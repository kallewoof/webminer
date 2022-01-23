// Copyright (c) 2022 Mark Friedenbach
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#ifndef WALLET_H

#include <stdint.h>

#include <string>

#include "crypto/sha256.h"
#include "uint256.h"

struct SecretWebcash {
    uint256 sk;
    int64_t amount;
};

std::string to_string(const SecretWebcash& esk);

struct PublicWebcash {
    uint256 pk;
    int64_t amount;

    PublicWebcash(const SecretWebcash& esk)
        : amount(esk.amount)
    {
        CSHA256()
            .Write(esk.sk.data(), esk.sk.size())
            .Finalize(pk.data());
    }
};

std::string to_string(const PublicWebcash& epk);

#endif // WALLET_H

// End of File