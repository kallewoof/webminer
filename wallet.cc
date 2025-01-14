// Copyright (c) 2022 Mark Friedenbach
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "wallet.h"

#include "webcash.h"

#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <stdint.h>

#include "absl/flags/declare.h"
#include "absl/flags/flag.h"

ABSL_DECLARE_FLAG(std::string, server);

#include "absl/strings/str_cat.h"

#include "absl/time/clock.h"
#include "absl/time/time.h"

#include "boost/filesystem.hpp"
#include "boost/filesystem/fstream.hpp"
#include "boost/interprocess/sync/file_lock.hpp"

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

#include "sqlite3.h"

#include <univalue.h>

#include "random.h"

// We group outputs based on their use.  There are currently four categories of
// webcash recognized by the wallet:
enum HashType : int {
    // Pre-generated key that hasn't yet been used for any purpose.  To make
    // backups possible and to minimize the chance of losing funds if/when
    // wallet corruption occurs, the wallet maintains a pool of pre-generated
    // secrets.  These are allocated and used, as needed, in FIFO order.
    UNUSED = -1,

    // Outputs added via explicit import.  These are shown as visible, discrete
    // inputs to the wallet.  The wallet always redeems received webcash upon
    // import under the assumption that the imported secret value is still known
    // to others or otherwise not secure.
    RECEIVE = 0,

    // Outputs generated as payments to others.  These are intended to be
    // immediately claimed by the other party, but we keep the key in this
    // wallet in case there are problems completing the transaction.
    PAYMENT = 1,

    // Internal webcash generated either to redeem payments or mined webcash,
    // change from a payment, or the consolidation of such outputs.  These
    // outputs count towards the current balance of the wallet, but aren't shown
    // explicitly.
    CHANGE = 2,

    // Outputs generated via a mining report.  These are seen as visible inputs
    // to a wallet, aggregated as "mining income."  The wallet always redeems
    // mining inputs for change immediately after generation, in case the mining
    // reports (which contain the secret) are made public.
    MINING = 3,
};

class BindParameterVisitor
{
protected:
    sqlite3_stmt* m_stmt;
    int m_index;

public:
    BindParameterVisitor(sqlite3_stmt* stmt, int index) : m_stmt(stmt), m_index(index) {}

    int operator()(const SqlNull& v) const { return sqlite3_bind_null(m_stmt, m_index); }
    int operator()(const SqlBool& v) const { return sqlite3_bind_int(m_stmt, m_index, !!v.b); }
    int operator()(const SqlInteger& v) const { return sqlite3_bind_int64(m_stmt, m_index, v.i); }
    int operator()(const SqlFloat& v) const { return sqlite3_bind_double(m_stmt, m_index, v.d); }
    int operator()(const SqlText& v) const { return sqlite3_bind_text(m_stmt, m_index, v.s.c_str(), v.s.size(), SQLITE_STATIC); }
    int operator()(const SqlBlob& v) const { return sqlite3_bind_blob(m_stmt, m_index, v.vch.data(), v.vch.size(), SQLITE_STATIC); }
};

std::string to_string(const SqlValue& v)
{
    using std::to_string;
    if (std::holds_alternative<SqlNull>(v)) {
        return "NULL";
    }
    if (std::holds_alternative<SqlBool>(v)) {
        return std::get<SqlBool>(v).b ? "TRUE" : "FALSE";
    }
    if (std::holds_alternative<SqlInteger>(v)) {
        return to_string(std::get<SqlInteger>(v).i);
    }
    if (std::holds_alternative<SqlFloat>(v)) {
        return to_string(std::get<SqlFloat>(v).d);
    }
    if (std::holds_alternative<SqlText>(v)) {
        std::stringstream ss;
        ss << std::quoted(std::get<SqlText>(v).s, '\'', '\'');
        return ss.str();
    }
    if (std::holds_alternative<SqlBlob>(v)) {
        const std::vector<unsigned char>& vch = std::get<SqlBlob>(v).vch;
        return absl::StrCat("x'", absl::BytesToHexString(std::string_view((const char*)vch.data(), vch.size())), "'");
    }
    return "unknown";
}

bool Wallet::ExecuteSql(const std::string& sql, const SqlParams& params)
{
    const char* head = sql.c_str();
    const char* tail = sql.c_str() + sql.size();
    while (head != tail) {
        // Parse next SQL statment
        sqlite3_stmt* stmt;
        size_t size = tail - head;
        int res = sqlite3_prepare_v2(m_db, head, size, &stmt, &tail);
        size = tail - head; // tail is set to the end of the current statement
        if (res != SQLITE_OK) {
            std::cerr << "Unable to prepare SQL statement [\"" << head << "\"]: " << sqlite3_errstr(res) << " (" << std::to_string(res) << ")" << std::endl;
            return false;
        }
        // Bind parameters
        for (const auto& bind : params) {
            const std::string key(":" + bind.first);
            int index = sqlite3_bind_parameter_index(stmt, key.c_str());
            if (index) {
                res = std::visit(BindParameterVisitor(stmt, index), bind.second);
                if (res != SQLITE_OK) {
                    std::cerr << "Unable to bind ':" << bind.first << "' in SQL statement [\"" << sqlite3_sql(stmt) << "\"] to " << to_string(bind.second) << ": " << sqlite3_errstr(res) << " (" << to_string(res) << ")" << std::endl;
                    sqlite3_finalize(stmt);
                    return false;
                }
            }
        }
        // Execute statement
        res = sqlite3_step(stmt);
        if (res != SQLITE_DONE) {
            std::cerr << "Running SQL statement [\"" << sqlite3_expanded_sql(stmt) << "\"] returned unexpected status code: " << sqlite3_errstr(res) << " (" << to_string(res) << ")" << std::endl;;
            sqlite3_finalize(stmt);
            return false;
        }
        sqlite3_finalize(stmt);
        // Set [head, tail) to point past the last statement executed before
        // continuing the loop:
        head = tail;
        tail = sql.c_str() + sql.size();
    }
    return true;
}

void Wallet::UpgradeDatabase()
{
    const std::string sql =
        "CREATE TABLE IF NOT EXISTS 'terms' ("
            "'id' INTEGER PRIMARY KEY NOT NULL,"
            "'body' TEXT UNIQUE NOT NULL,"
            "'timestamp' INTEGER NOT NULL);"
        "CREATE TABLE IF NOT EXISTS 'secret' ("
            "'id' INTEGER PRIMARY KEY NOT NULL,"
            "'timestamp' INTEGER NOT NULL,"
            "'secret' TEXT UNIQUE NOT NULL,"
            "'mine' INTEGER NOT NULL,"
            "'sweep' INTEGER NOT NULL);"
        "CREATE TABLE IF NOT EXISTS 'output' ("
            "'id' INTEGER PRIMARY KEY NOT NULL,"
            "'timestamp' INTEGER NOT NULL,"
            "'hash' BLOB NOT NULL,"
            "'secret_id' INTEGER,"
            "'amount' INTEGER NOT NULL,"
            "'spent' INTEGER NOT NULL,"
            "FOREIGN KEY('secret_id') REFERENCES 'secret'('id'));"
        "CREATE TABLE IF NOT EXISTS 'hdroot' ("
            "'id' INTEGER PRIMARY KEY NOT NULL,"
            "'timestamp' INTEGER NOT NULL,"
            "'version' INTEGER NOT NULL,"
            "'secret' BLOB NOT NULL,"
            "UNIQUE('version','secret'));"
        "CREATE TABLE IF NOT EXISTS 'hdchain' ("
            "'id' INTEGER PRIMARY KEY NOT NULL,"
            "'hdroot_id' INTEGER NOT NULL,"
            "'chaincode' INTEGER UNSIGNED NOT NULL," // <-- The upper 62 bits
            "'mine' INTEGER NOT NULL,"      // <-- The lower 2 bits of chaincode
            "'sweep' INTEGER NOT NULL,"     //     come from these two fields.
            "'mindepth' INTEGER UNSIGNED NOT NULL,"
            "'maxdepth' INTEGER UNSIGNED NOT NULL,"
            "FOREIGN KEY('hdroot_id') REFERENCES 'hdroot'('id'),"
            "UNIQUE('hdroot_id','chaincode','mine','sweep'));"
        "CREATE TABLE IF NOT EXISTS 'hdkey' ("
            "'id' INTEGER PRIMARY KEY NOT NULL,"
            "'hdchain_id' INTEGER NOT NULL,"
            "'depth' INTEGER UNSIGNED NOT NULL,"
            "'secret_id' INTEGER UNIQUE NOT NULL,"
            "FOREIGN KEY('hdchain_id') REFERENCES 'hdchain'('id'),"
            "FOREIGN KEY('secret_id') REFERENCES 'secret'('id'),"
            "UNIQUE('hdchain_id','depth'));";
    if (!ExecuteSql(sql, {})) {
        throw std::runtime_error("Unable to create database tables.  See error log for details.");
    }
}

void Wallet::GetOrCreateHDRoot()
{
    using std::to_string;

    int count = -1;
    {
        const std::string sql = "SELECT COUNT(1) FROM 'hdroot';";
        sqlite3_stmt* stmt;
        int res = sqlite3_prepare_v2(m_db, sql.c_str(), sql.size(), &stmt, nullptr);
        if (res != SQLITE_OK) {
            std::string msg(absl::StrCat("Unable to prepare SQL statement [\"", sql, "\"]: ", sqlite3_errstr(res), " (", std::to_string(res), ")"));
            std::cerr << msg << std::endl;
            throw std::runtime_error(msg);
        }
        res = sqlite3_step(stmt);
        if (res != SQLITE_ROW) {
            std::string msg(absl::StrCat("Running SQL statement [\"", sql, "\"] returned unexpected status code: ", sqlite3_errstr(res), " (", std::to_string(res), ")"));
            std::cerr << msg << std::endl;
            sqlite3_finalize(stmt);
            throw std::runtime_error(msg);
        }
        count = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }

    if (count == 0) {
        std::cout << "Generating master secret for wallet." << std::endl;
        const int64_t timestamp = absl::ToUnixSeconds(absl::Now());
        GetStrongRandBytes(m_hdroot.begin(), 32);

        {
            std::string line = absl::StrCat(to_string(timestamp), " hdroot ", absl::BytesToHexString(absl::string_view((const char*)m_hdroot.begin(), 32)), " version=1");
            boost::filesystem::ofstream bak(m_logfile.string(), boost::filesystem::ofstream::app);
            if (!bak) {
                std::string msg("Unable to open/create wallet recovery file to save wallet master key.");
                std::cerr << msg << std::endl;
                throw std::runtime_error(msg);
            } else {
                bak << line << std::endl;
                bak.flush();
            }
        }

        const std::string sql =
            "BEGIN TRANSACTION;"
            ""
            "INSERT OR IGNORE INTO hdroot ('timestamp','version','secret')"
            "VALUES(:timestamp,1,:secret);"
            ""
            "INSERT OR IGNORE INTO hdchain ('hdroot_id','chaincode','mine','sweep','mindepth','maxdepth')"
            "VALUES((SELECT id FROM 'hdroot' WHERE secret=:secret),0,FALSE,FALSE,0,0),"
                  "((SELECT id FROM 'hdroot' WHERE secret=:secret),0,FALSE,TRUE,0,0),"
                  "((SELECT id FROM 'hdroot' WHERE secret=:secret),0,TRUE,FALSE,0,0),"
                  "((SELECT id FROM 'hdroot' WHERE secret=:secret),0,TRUE,TRUE,0,0);"
            ""
            "COMMIT;";
        SqlParams params;
        params["timestamp"] = SqlInteger(timestamp);
        params["secret"] = SqlBlob(m_hdroot.begin(), m_hdroot.end());
        if (!ExecuteSql(sql, params)) {
            throw std::runtime_error("Unable to insert master secret into database.  See error log for details.");
        }
    }

    if (count == 0 || count == 1) {
        if (count == 1) {
            std::cout << "Loading master secret from wallet." << std::endl;
        }
        const std::string sql = "SELECT id,version,secret FROM 'hdroot' LIMIT 1;";
        sqlite3_stmt* stmt;
        int res = sqlite3_prepare_v2(m_db, sql.c_str(), sql.size(), &stmt, nullptr);
        if (res != SQLITE_OK) {
            std::string msg(absl::StrCat("Unable to prepare SQL statement [\"", sql, "\"]: ", sqlite3_errstr(res), " (", std::to_string(res), ")"));
            std::cerr << msg << std::endl;
            throw std::runtime_error(msg);
        }
        res = sqlite3_step(stmt);
        if (res != SQLITE_ROW) {
            std::string msg(absl::StrCat("Running SQL statement [\"", sql, "\"] returned unexpected status code: ", sqlite3_errstr(res), " (", std::to_string(res), ")"));
            std::cerr << msg << std::endl;
            sqlite3_finalize(stmt);
            throw std::runtime_error(msg);
        }
        int hdroot_id = sqlite3_column_int(stmt, 0);
        int version = sqlite3_column_int(stmt, 1);
        if (version != 1) {
            std::string msg(absl::StrCat("Wallet contains HD root with unrecognized version(", to_string(version), "  Not sure what to do."));
            std::cerr << msg << std::endl;
            sqlite3_finalize(stmt);
            throw std::runtime_error(msg);
        }
        size_t len = sqlite3_column_bytes(stmt, 2);
        if (len < 16 || 32 < len) {
            std::string msg(absl::StrCat("Expected between 16-32 bytes for HD root secret value.  Got ", to_string(len), " bytes.  Not sure what to do."));
            std::cerr << msg << std::endl;
            sqlite3_finalize(stmt);
            throw std::runtime_error(msg);
        }
        const unsigned char* data = (const unsigned char*)sqlite3_column_blob(stmt, 2);
        if (!data) {
            std::string msg("Expected data pointer for HD root secret value.  Got NULL instead.  Not sure what to do.");
            std::cerr << msg << std::endl;
            sqlite3_finalize(stmt);
            throw std::runtime_error(msg);
        }
        m_hdroot_id = hdroot_id;
        std::copy(data, data + len, m_hdroot.begin());
        if (len < m_hdroot.size()) {
            std::fill(m_hdroot.begin() + len, m_hdroot.end(), 0);
        }
        sqlite3_finalize(stmt);
    }

    else {
        std::string msg("Wallet contains more than one HD root secret.  Not sure what to do.");
        std::cerr << msg << std::endl;
        throw std::runtime_error(msg);
    }
}

Wallet::Wallet(const boost::filesystem::path& path)
    : m_logfile(path)
{
    // The caller can either give the path to one of the wallet files (the
    // recovery log or the sqlite3 database file), or to the shared basename of
    // these files.
    m_logfile.replace_extension(".bak");

    boost::filesystem::path dbfile(path);
    dbfile.replace_extension(".db");
    // Create the database file if it doesn't exist already, so that we can use
    // inter-process file locking primitives on it.  Note that an empty file is
    // a valid, albeit empty sqlite3 database.
    {
        boost::filesystem::ofstream db(dbfile.string(), boost::filesystem::ofstream::app);
        db.flush();
    }
    m_db_lock = boost::interprocess::file_lock(dbfile.c_str());
    if (!m_db_lock.try_lock()) {
        std::string msg("Unable to lock wallet database; wallet is in use by another process.");
        std::cerr << msg << std::endl;
        throw std::runtime_error(msg);
    }

    int error = sqlite3_open_v2(dbfile.c_str(), &m_db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_EXRESCODE, nullptr);
    if (error != SQLITE_OK) {
        m_db_lock.unlock();
        std::string msg(absl::StrCat("Unable to open/create wallet database file: ", sqlite3_errstr(error), " (", std::to_string(error), ")"));
        std::cerr << msg << std::endl;
        throw std::runtime_error(msg);
    }
    UpgradeDatabase();
    GetOrCreateHDRoot();

    // Touch the wallet file, which will create it if it doesn't already exist.
    // The file locking primitives assume that the file exists, so we need to
    // create here first.  It also allows the user to see the file even before
    // any wallet operations have been performed.
    {
        // This operation isn't protected by a filesystem lock, but that
        // shouldn't be an issue because it doesn't do anything else the file
        // didn't exist in the first place.
        boost::filesystem::ofstream bak(m_logfile.string(), boost::filesystem::ofstream::app);
        if (!bak) {
            sqlite3_close_v2(m_db); m_db = nullptr;
            m_db_lock.unlock();
            std::string msg(absl::StrCat("Unable to open/create wallet recovery file"));
            std::cerr << msg << std::endl;
            throw std::runtime_error(msg);
        }
        bak.flush();
    }
}

Wallet::~Wallet()
{
    // Wait for other threads using the wallet to finish up.
    const std::lock_guard<std::mutex> lock(m_mut);
    // No errors are expected when closing the database file, but if there is
    // then that might be an indication of a serious bug or data loss the user
    // should know about.
    int error = sqlite3_close_v2(m_db); m_db = nullptr;
    if (error != SQLITE_OK) {
        std::cerr << "WARNING: sqlite3 returned error code " << sqlite3_errstr(error) << " (" << std::to_string(error) << ") when attempting to close database file of wallet.  Data loss may have occured." << std::endl;
    }
    // Release our filesystem lock on the wallet.
    m_db_lock.unlock();
    // Secure-erase the master secret from memory
    memory_cleanse(m_hdroot.begin(), m_hdroot.size());
}

std::string to_string(HashType type)
{
    if (type == HashType::UNUSED) {
        return "unused";
    }
    if (type == HashType::PAYMENT) {
        return "pay";
    }
    if (type == HashType::RECEIVE) {
        return "recieve";
    }
    if (type == HashType::CHANGE) {
        return "change";
    }
    if (type == HashType::MINING) {
        return "mining";
    }
    return "unknown";
}

static HashType get_hash_type(bool mine, bool sweep)
{
    if (!mine && !sweep) {
        return HashType::PAYMENT;
    }
    if (!mine && sweep) {
        return HashType::RECEIVE;
    }
    if (mine && !sweep) {
        return HashType::CHANGE;
    }
    if (mine && sweep) {
        return HashType::MINING;
    }
    return HashType::UNUSED;
}

WalletSecret Wallet::ReserveSecret(absl::Time _timestamp, bool mine, bool sweep)
{
    const int64_t chaincode = 0;

    // Timestamps in the database are recorded as seconds since the UNIX epoch.
    const int64_t timestamp = absl::ToUnixSeconds(_timestamp);

    int hdchain_id = -1;
    int64_t depth = -1;
    {
        const std::string sql =
            "SELECT id,maxdepth "
              "FROM 'hdchain' "
             "WHERE hdroot_id=:hdroot_id "
               "AND chaincode=:chaincode "
               "AND mine=:mine "
               "AND sweep=:sweep "
            "LIMIT 1;";
        sqlite3_stmt* stmt;
        int res = sqlite3_prepare_v2(m_db, sql.c_str(), sql.size(), &stmt, nullptr);
        if (res != SQLITE_OK) {
            std::string msg(absl::StrCat("Unable to prepare SQL statement [\"", sql, "\"]: ", sqlite3_errstr(res), " (", to_string(res), ")"));
            std::cerr << msg << std::endl;
            throw std::runtime_error(msg);
        }
        res = sqlite3_bind_int(stmt, sqlite3_bind_parameter_index(stmt, ":hdroot_id"), m_hdroot_id);
        if (res != SQLITE_OK) {
            std::string msg(absl::StrCat("Unable to bind ':hdroot_id' in SQL statement [\"", sqlite3_sql(stmt), "\"] to ", m_hdroot_id, ": ", sqlite3_errstr(res), " (", to_string(res), ")"));
            std::cerr << msg << std::endl;
            sqlite3_finalize(stmt);
            throw std::runtime_error(msg);
        }
        res = sqlite3_bind_int64(stmt, sqlite3_bind_parameter_index(stmt, ":chaincode"), chaincode);
        if (res != SQLITE_OK) {
            std::string msg(absl::StrCat("Unable to bind ':hdroot_id' in SQL statement [\"", sqlite3_sql(stmt), "\"] to ", to_string(chaincode), ": ", sqlite3_errstr(res), " (", to_string(res), ")"));
            std::cerr << msg << std::endl;
            sqlite3_finalize(stmt);
            throw std::runtime_error(msg);
        }
        res = sqlite3_bind_int(stmt, sqlite3_bind_parameter_index(stmt, ":mine"), !!mine);
        if (res != SQLITE_OK) {
            std::string msg(absl::StrCat("Unable to bind ':mine' in SQL statement [\"", sqlite3_sql(stmt), "\"] to ", (mine ? "TRUE" : "FALSE"), ": ", sqlite3_errstr(res), " (", to_string(res), ")"));
            std::cerr << msg << std::endl;
            sqlite3_finalize(stmt);
            throw std::runtime_error(msg);
        }
        res = sqlite3_bind_int(stmt, sqlite3_bind_parameter_index(stmt, ":sweep"), !!sweep);
        if (res != SQLITE_OK) {
            std::string msg(absl::StrCat("Unable to bind ':sweep' in SQL statement [\"", sqlite3_sql(stmt), "\"] to ", (mine ? "TRUE" : "FALSE"), ": ", sqlite3_errstr(res), " (", to_string(res), ")"));
            std::cerr << msg << std::endl;
            sqlite3_finalize(stmt);
            throw std::runtime_error(msg);
        }
        res = sqlite3_step(stmt);
        if (res != SQLITE_ROW) {
            std::string msg = absl::StrCat("Running SQL statement [\"", sqlite3_expanded_sql(stmt), "\"] returned unexpected status code: ", sqlite3_errstr(res), " (", to_string(res), ")");
            std::cerr << msg << std::endl;
            sqlite3_finalize(stmt);
            throw std::runtime_error(msg);
        }
        hdchain_id = sqlite3_column_int(stmt, 0);
        if (hdchain_id < 0) {
            std::string msg(absl::StrCat("Current HD chain id is negative.  Not sure what to do."));
            std::cerr << msg << std::endl;
            sqlite3_finalize(stmt);
            throw std::runtime_error(msg);
        }
        depth = sqlite3_column_int64(stmt, 1);
        if (depth < 0) {
            std::string msg(absl::StrCat("Current HD chain depth is negative.  Not sure what to do."));
            std::cerr << msg << std::endl;
            sqlite3_finalize(stmt);
            throw std::runtime_error(msg);
        }
        sqlite3_finalize(stmt);
    }

    const std::string tag_str = "webcashwalletv1";
    uint256 tag;
    CSHA256()
        .Write((const unsigned char*)tag_str.c_str(), tag_str.size())
        .Finalize(tag.begin());
    std::array<unsigned char, 8> chaincode_bytes = {
        static_cast<unsigned char>((chaincode >> 54) & 0xff),
        static_cast<unsigned char>((chaincode >> 46) & 0xff),
        static_cast<unsigned char>((chaincode >> 38) & 0xff),
        static_cast<unsigned char>((chaincode >> 30) & 0xff),
        static_cast<unsigned char>((chaincode >> 22) & 0xff),
        static_cast<unsigned char>((chaincode >> 14) & 0xff),
        static_cast<unsigned char>((chaincode >> 6) & 0xff),
        static_cast<unsigned char>((chaincode << 2) & 0xfc),
    };
    if (!mine && sweep) {
        chaincode_bytes.back() |= 0;
    } else
    if (!mine && !sweep) {
        chaincode_bytes.back() |= 1;
    } else
    if (mine && !sweep) {
        chaincode_bytes.back() |= 2;
    } else
    if (mine && sweep) {
        chaincode_bytes.back() |= 3;
    }
    std::array<unsigned char, 8> depth_bytes = {
        static_cast<unsigned char>((depth >> 56) & 0xff),
        static_cast<unsigned char>((depth >> 48) & 0xff),
        static_cast<unsigned char>((depth >> 40) & 0xff),
        static_cast<unsigned char>((depth >> 32) & 0xff),
        static_cast<unsigned char>((depth >> 24) & 0xff),
        static_cast<unsigned char>((depth >> 16) & 0xff),
        static_cast<unsigned char>((depth >> 8) & 0xff),
        static_cast<unsigned char>(depth & 0xff),
    };
    uint256 secret;
    CSHA256()
        .Write(tag.begin(), 32)
        .Write(tag.begin(), 32)
        .Write(m_hdroot.begin(), 32)
        .Write(chaincode_bytes.data(), chaincode_bytes.size())
        .Write(depth_bytes.data(), depth_bytes.size())
        .Finalize(secret.begin());
    SecureString sk(absl::BytesToHexString(absl::string_view((const char*)secret.begin(), secret.size())));
    memory_cleanse(secret.begin(), secret.size());

    int secret_id = -1;
    {
        const std::string sql =
            "BEGIN TRANSACTION;"
            ""
            "INSERT OR IGNORE INTO secret ('timestamp','secret','mine','sweep')"
            "VALUES(:timestamp,:secret,:mine,:sweep);"
            "UPDATE secret SET mine = mine & :mine WHERE secret = :secret;"
            "UPDATE secret SET sweep = sweep | :sweep WHERE secret = :secret;"
            ""
            "INSERT OR IGNORE INTO hdkey ('hdchain_id','depth','secret_id')"
            "VALUES(:hdchain_id,:depth,(SELECT id FROM 'secret' WHERE secret = :secret));"
            ""
            "UPDATE 'hdchain' SET maxdepth = maxdepth + 1 "
            "WHERE id = :hdchain_id;"
            ""
            "COMMIT;";
        SqlParams params;
        params["timestamp"] = SqlInteger(timestamp);
        params["secret"] = SqlText(sk);
        params["mine"] = SqlBool(mine);
        params["sweep"] = SqlBool(sweep);
        params["hdchain_id"] = SqlInteger(hdchain_id);
        params["depth"] = SqlInteger(depth);
        if (!ExecuteSql(sql, params)) {
            throw std::runtime_error("Unable to insert secret into database.  See error log for details.");
        }
    }

    WalletSecret wsecret;
    wsecret.id = secret_id;
    wsecret.timestamp = _timestamp;
    wsecret.secret = sk;
    wsecret.mine = mine;
    wsecret.sweep = sweep;

    return wsecret;
}

int Wallet::AddSecretToWallet(absl::Time _timestamp, const SecretWebcash &sk, bool mine, bool sweep)
{
    using std::to_string;
    int result = true;

    // Timestamps in the database are recorded as seconds since the UNIX epoch.
    const int64_t timestamp = absl::ToUnixSeconds(_timestamp);

    // First write the key to the wallet recovery file.
    {
        std::string line = absl::StrCat(to_string(timestamp), " ", to_string(get_hash_type(mine, sweep)), " ", to_string(sk));
        boost::filesystem::ofstream bak(m_logfile.string(), boost::filesystem::ofstream::app);
        if (!bak) {
            std::cerr << "WARNING: Unable to open/create wallet recovery file to save key prior to insertion: \"" << line << "\".  BACKUP THIS KEY NOW TO AVOID DATA LOSS!" << std::endl;
            // We do not return 0 here even though there was an error writing to
            // the recovery log, because we can still attempt to save the key to
            // the wallet.
            result = false;
        } else {
            bak << line << std::endl;
            bak.flush();
        }
    }

    // Then attempt to write the key to the wallet database
    const std::string sql =
        "BEGIN TRANSACTION;"
        ""
        "INSERT OR IGNORE INTO secret ('timestamp','secret','mine','sweep')"
        "VALUES(:timestamp,:secret,:mine,:sweep);"
        ""
        "UPDATE secret SET mine = mine & :mine WHERE secret = :secret;"
        "UPDATE secret SET sweep = sweep | :sweep WHERE secret = :secret;"
        ""
        "COMMIT;";
    SqlParams params;
    params["timestamp"] = SqlInteger(timestamp);
    params["secret"] = SqlText(sk.sk);
    params["mine"] = SqlBool(mine);
    params["sweep"] = SqlBool(sweep);
    result = ExecuteSql(sql, params) && result;

    return result ? sqlite3_last_insert_rowid(m_db) : 0;
}

int Wallet::AddOutputToWallet(absl::Time _timestamp, const PublicWebcash& pk, int secret_id, bool spent)
{
    using std::to_string;

    // Timestamps in the database are recorded as seconds since the UNIX epoch.
    const int64_t timestamp = absl::ToUnixSeconds(_timestamp);

    // Attempt to write the output record to the database.
    const std::string sql =
        "INSERT INTO output ('timestamp','hash','secret_id','amount','spent')"
        "VALUES(:timestamp,:hash,:secret_id,:amount,:spent);";
    SqlParams params;
    params["timestamp"] = SqlInteger(timestamp);
    params["hash"] = SqlBlob(pk.pk.begin(), pk.pk.end());
    if (secret_id) {
        params["secret_id"] = SqlInteger(secret_id);
    } else {
        params["secret_id"] = SqlNull();
    }
    params["amount"] = SqlInteger(pk.amount.i64);
    params["spent"] = SqlBool(spent);
    if (!ExecuteSql(sql, params)) {
        return 0;
    }

    return sqlite3_last_insert_rowid(m_db);
}

std::vector<std::pair<WalletSecret, int>> Wallet::ReplaceWebcash(absl::Time timestamp, std::vector<WalletOutput>& inputs, const std::vector<std::pair<WalletSecret, Amount>>& outputs)
{
    using std::to_string;

    Amount total_in = 0;
    UniValue in(UniValue::VARR);
    if (inputs.empty()) {
        std::cerr << "No inputs provided for replacement." << std::endl;
        return {};
    }
    for (const WalletOutput& webcash : inputs) {
        if (!webcash.secret) {
            std::cerr << "Unable to replace output without corresponding secret: " << to_string(PublicWebcash(webcash.hash, webcash.amount)) << std::endl;
            return {};
        }
        if (webcash.amount.i64 < 1) {
            std::cerr << "Invalid amount for replacement intput: " << to_string(PublicWebcash(webcash.hash, webcash.amount)) << std::endl;
        }
        if (webcash.spent) {
            std::cerr << "Replacement intput already spent: " << to_string(PublicWebcash(webcash.hash, webcash.amount)) << std::endl;
            return {};
        }
        in.push_back(std::string(to_string(SecretWebcash(webcash.secret->secret, webcash.amount)).c_str()));
        total_in += webcash.amount;
    }

    Amount total_out = 0;
    UniValue out(UniValue::VARR);
    if (outputs.empty()) {
        std::cerr << "No outputs provided for replacement." << std::endl;
        return {};
    }
    for (const std::pair<WalletSecret, Amount>& webcash : outputs) {
        if (webcash.second.i64 < 1) {
            std::cerr << "Invalid amount for replacement output: " << to_string(PublicWebcash(SecretWebcash(webcash.first.secret, webcash.second))) << std::endl;
            return {};
        }
        out.push_back(std::string(to_string(SecretWebcash(webcash.first.secret, webcash.second)).c_str()));
        total_out += webcash.second;
    }

    if (total_in != total_out) {
        std::cerr << "Invalid replacement: sum(inputs) != sum(outputs) [" << to_string(total_in) << " != " << to_string(total_out) << "]" << std::endl;
        return {};
    }

    // Acceptance of terms of service is hard-coded here because it is checked
    // for on startup.
    UniValue legalese(UniValue::VOBJ);
    legalese.push_back(std::make_pair("terms", true));

    UniValue replace(UniValue::VOBJ);
    replace.push_back(std::make_pair("webcashes", in));
    replace.push_back(std::make_pair("new_webcashes", out));
    replace.push_back(std::make_pair("legalese", legalese));

    // Submit replacement
    const std::string server = absl::GetFlag(FLAGS_server);
    httplib::Client cli(server);
    cli.set_read_timeout(60, 0); // 60 seconds
    cli.set_write_timeout(60, 0); // 60 seconds
    auto r = cli.Post(
        "/api/v1/replace",
        replace.write(),
        "application/json");

    // Handle network errors by aborting further processing
    if (!r) {
        std::cerr << "Error: returned invalid response to Replace request: " << r.error() << std::endl;
        std::cerr << "Possible transient error, or server timeout?  Cannot proceed." << std::endl;
        return {};
    }

    // Parse response.
    UniValue o;
    o.read(r->body);

    // Report server rejection to the user.
    if (r->status != 200) {
        std::cerr << "Error: returned invalid response to Replace request: status_code=" << r->status << ", text='" << r->body << "'" << std::endl;
        return {};
    }

    // Mark each input as spent in the database.
    {
        for (WalletOutput& webcash : inputs) {
            // Update the object.
            webcash.spent = true;
            // Update the database.
            const std::string sql =
                "UPDATE 'output' SET spent=TRUE WHERE id=:output_id;";
            SqlParams params;
            params["output_id"] = SqlInteger(webcash.id);
            if (!ExecuteSql(sql, params)) {
                std::cerr << "Unable to mark output as spent.  See error log for details." << std::endl;
                continue;
            }
        }
    }

    // Create record for each output.
    std::vector<std::pair<WalletSecret, int>> ret;
    for (const std::pair<WalletSecret, Amount>& webcash : outputs) {
        // Create database record.
        int id = AddOutputToWallet(timestamp, PublicWebcash(SecretWebcash(webcash.first.secret, webcash.second)), webcash.first.id, false);
        if (!id) {
            std::cerr << "Error creating database record for replacement output: " << to_string(PublicWebcash(SecretWebcash(webcash.first.secret, webcash.second))) << std::endl;
            continue;
        }

        // Save the database id for caller.
        ret.push_back(std::make_pair(webcash.first, id));
    }

    return ret;
}

bool Wallet::Insert(const SecretWebcash& sk, bool mine)
{
    using std::to_string;
    const std::lock_guard<std::mutex> lock(m_mut);

    // The database records the timestamp of an insertion
    const absl::Time now = absl::Now();

    // Insert secret into the wallet db.
    int secret_id = AddSecretToWallet(now, sk, mine, true);
    if (!secret_id) {
        std::cerr << "Error adding secret to wallet; unable to proceed with insertion." << std::endl;
        return false;
    }

    WalletSecret wsecret;
    wsecret.id = secret_id;
    wsecret.timestamp = now;
    wsecret.secret = sk.sk;
    wsecret.mine = mine;
    wsecret.sweep = true;

    // Insert output record into the wallet db.
    PublicWebcash pk(sk);
    int output_id = AddOutputToWallet(now, pk, secret_id, false);
    if (!output_id) {
        std::cerr << "Error adding output to wallet; unable to proceed with insertion." << std::endl;
        return false;
    }

    WalletOutput woutput;
    woutput.id = output_id;
    woutput.timestamp = now;
    woutput.hash = pk.pk;
    woutput.secret = std::make_unique<WalletSecret>(wsecret);
    woutput.amount = pk.amount;
    woutput.spent = false;

    // Generate change address.
    // FIXME: This is breaking with webcash wallet standards; sweep shoud really
    //        be false here.  The reason we do it this way is a bit of a
    //        hack/workaround. Until webminer has full wallet support, it is
    //        easiest for users to import their root key into webcasa and use
    //        that as their wallet.  However any payments made in webcasa will
    //        use change addresses, which could potentially result in webminer
    //        insertions failing due to secret reuse.
    //
    //        The workaround is to use HashType::MINING for change addresses
    //        when replacing secrets.  This is not what the HashType::MINING
    //        chain code is meant to be used for.  It is meant to be the way in
    //        which mining payload secrets are generated, hence why sweep=true.
    //        However webminer currently uses random secrets for the mining
    //        payload, and until a proper wallet is implemented this at least
    //        achieves domain separation from webminer and webcasa.
    //
    //                                                         should be false <==>
    WalletSecret wchange = ReserveSecret(now, /* mine = */ true, /* sweep = */ true);
    SecretWebcash change(wchange.secret, sk.amount);

    std::vector<WalletOutput> inputs;
    inputs.emplace_back(std::move(woutput));

    std::vector<std::pair<WalletSecret, Amount>> outputs;
    outputs.emplace_back(wchange, change.amount);

    std::vector<std::pair<WalletSecret, int>> res = ReplaceWebcash(now, inputs, outputs);
    if (res.size() != 1) {
        std::cerr << "Error executing replacement on server; keys are secured in wallet, but assuming replacement did not go through." << std::endl;
        return false;
    }

    return true;
}

bool Wallet::HaveAcceptedTerms()
{
    const std::lock_guard<std::mutex> lock(m_mut);
    static const std::string stmt = "SELECT EXISTS(SELECT 1 FROM 'terms')";
    sqlite3_stmt* have_any_terms;
    int res = sqlite3_prepare_v2(m_db, stmt.c_str(), stmt.size(), &have_any_terms, nullptr);
    if (res != SQLITE_OK) {
        std::string msg(absl::StrCat("Unable to prepare SQL statement [\"", stmt, "\"]: ", sqlite3_errstr(res), " (", std::to_string(res), ")"));
        std::cerr << msg << std::endl;
        throw std::runtime_error(msg);
    }
    res = sqlite3_step(have_any_terms);
    if (res != SQLITE_ROW) {
        std::string msg(absl::StrCat("Expected a result from executing SQL statement [\"", sqlite3_expanded_sql(have_any_terms), "\"] not: ", sqlite3_errstr(res), " (", std::to_string(res), ")"));
        std::cerr << msg << std::endl;
        sqlite3_finalize(have_any_terms);
        throw std::runtime_error(msg);
    }
    bool any = !!sqlite3_column_int(have_any_terms, 0);
    sqlite3_finalize(have_any_terms);
    return any;
}

bool Wallet::AreTermsAccepted(const std::string& terms)
{
    const std::lock_guard<std::mutex> lock(m_mut);
    static const std::string stmt = "SELECT EXISTS(SELECT 1 FROM 'terms' WHERE body=?)";
    sqlite3_stmt* have_terms;
    int res = sqlite3_prepare_v2(m_db, stmt.c_str(), stmt.size(), &have_terms, nullptr);
    if (res != SQLITE_OK) {
        std::string msg(absl::StrCat("Unable to prepare SQL statement [\"", stmt, "\"]: ", sqlite3_errstr(res), " (", std::to_string(res), ")"));
        std::cerr << msg << std::endl;
        throw std::runtime_error(msg);
    }
    res = sqlite3_bind_text(have_terms, 1, terms.c_str(), terms.size(), SQLITE_STATIC);
    if (res != SQLITE_OK) {
        std::string msg(absl::StrCat("Unable to bind parameter 1 in SQL statement [\"", stmt, "\"]: ", sqlite3_errstr(res), " (", std::to_string(res), ")"));
        std::cerr << msg << std::endl;
        sqlite3_finalize(have_terms);
        throw std::runtime_error(msg);
    }
    res = sqlite3_step(have_terms);
    if (res != SQLITE_ROW) {
        std::string msg(absl::StrCat("Expected a result from executing SQL statement [\"", sqlite3_expanded_sql(have_terms), "\"] not: ", sqlite3_errstr(res), " (", std::to_string(res), ")"));
        std::cerr << msg << std::endl;
        sqlite3_finalize(have_terms);
        throw std::runtime_error(msg);
    }
    bool have = !!sqlite3_column_int(have_terms, 0);
    sqlite3_finalize(have_terms);
    return have;
}

void Wallet::AcceptTerms(const std::string& terms)
{
    if (!AreTermsAccepted(terms)) {
        const std::lock_guard<std::mutex> lock(m_mut);
        static const std::string sql =
            "INSERT OR IGNORE INTO terms ('body','timestamp')"
            "VALUES(:body,:timestamp)";
        SqlParams params;
        params["body"] = SqlText(terms);
        int64_t timestamp = absl::ToUnixSeconds(absl::Now());
        params["timestamp"] = SqlInteger(timestamp);
        if (!ExecuteSql(sql, params)) {
            throw std::runtime_error("Unable to insert accepted terms into database.  See error log for details.");
        }
    }
}

// End of File
