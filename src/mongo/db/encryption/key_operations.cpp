/*======
This file is part of Percona Server for MongoDB.

Copyright (C) 2022-present Percona and/or its affiliates. All rights reserved.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the Server Side Public License, version 1,
    as published by MongoDB, Inc.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    Server Side Public License for more details.

    You should have received a copy of the Server Side Public License
    along with this program. If not, see
    <http://www.mongodb.com/licensing/server-side-public-license>.

    As a special exception, the copyright holders give permission to link the
    code of portions of this program with the OpenSSL library under certain
    conditions as described in each individual source file and distribute
    linked combinations including the program with the OpenSSL library. You
    must comply with the Server Side Public License in all respects for
    all of the code used other than as permitted herein. If you modify file(s)
    with this exception, you may extend this exception to your version of the
    file(s), but you are not obligated to do so. If you do not wish to do so,
    delete this exception statement from your version. If you delete this
    exception statement from all source files in the program, then also delete
    it in the license file.
======= */

#include "mongo/db/encryption/key_operations.h"

#include "mongo/db/encryption/encryption_kmip.h"
#include "mongo/db/encryption/encryption_options.h"
#include "mongo/db/encryption/encryption_vault.h"
#include "mongo/db/encryption/key.h"
#include "mongo/db/encryption/key_error.h"
#include "mongo/db/encryption/secret_string.h"
#include "mongo/util/assert_util_core.h"

namespace mongo::encryption {
std::optional<KeyKeyIdPair> ReadKeyFile::operator()() const {
    return KeyKeyIdPair{Key(detail::SecretString::readFromFile(_path.toString(), "encryption key")),
                        _path.clone()};
}

std::pair<std::string, std::uint64_t> ReadVaultSecret::_read(const VaultSecretId& id) const {
    return detail::vaultReadKey(id.path(), id.version());
}

std::optional<KeyKeyIdPair> ReadVaultSecret::operator()() const {
    if (auto [encodedKey, version] = _read(_id); !encodedKey.empty()) {
        return KeyKeyIdPair{Key(encodedKey), std::make_unique<VaultSecretId>(_id.path(), version)};
    }
    return std::nullopt;
}

std::unique_ptr<KeyId> SaveVaultSecret::operator()(const Key& k) const {
    return std::make_unique<VaultSecretId>(_secretPath,
                                           detail::vaultWriteKey(_secretPath, k.base64()));
}

std::optional<KeyKeyIdPair> ReadKmipKey::operator()() const {
    if (auto encodedKey = detail::kmipReadKey(_id.toString()); !encodedKey.empty()) {
        return KeyKeyIdPair{Key(encodedKey), _id.clone()};
    }
    return std::nullopt;
}

std::unique_ptr<KeyId> SaveKmipKey::operator()(const Key& k) const {
    return std::make_unique<KmipKeyId>(detail::kmipWriteKey(k.base64()));
}

std::unique_ptr<KeyOperationFactory> KeyOperationFactory::create(
    const EncryptionGlobalParams& params) {
    if (!params.encryptionKeyFile.empty()) {
        return std::make_unique<KeyFileOperationFactory>(params.encryptionKeyFile);
    } else if (!params.vaultServerName.empty()) {
        return std::make_unique<VaultSecretOperationFactory>(
            params.vaultRotateMasterKey, params.vaultSecret, params.vaultSecretVersion);
    } else if (!params.kmipServerName.empty()) {
        return std::make_unique<KmipKeyOperationFactory>(params.kmipRotateMasterKey,
                                                         params.kmipKeyIdentifier);
    }
    invariant(false && "Should not reach this point");
    return nullptr;
}

std::unique_ptr<SaveKey> KeyFileOperationFactory::createSave(const KeyId* configured) const {
    invariant(false && "Encryption key can not be saved to a file");
    return nullptr;
}

VaultSecretOperationFactory::VaultSecretOperationFactory(
    bool rotateMasterKey,
    const std::string& providedSecretPath,
    const std::optional<std::uint64_t>& providedSecretVersion)
    : _rotateMasterKey(rotateMasterKey),
      _configured(nullptr) {
    if (providedSecretVersion) {
        _provided = VaultSecretId(providedSecretPath, *providedSecretVersion);
    } else {
        _providedSecretPath = providedSecretPath;
    }
}

KmipKeyOperationFactory::KmipKeyOperationFactory(bool rotateMasterKey,
                                                 const std::string& providedKeyId)
    : _rotateMasterKey(rotateMasterKey),
      _configured(nullptr) {
    if (!providedKeyId.empty()) {
        _provided = KmipKeyId(providedKeyId);
    }
}

namespace detail {
template <typename T> struct Messages;

template <>
struct Messages<VaultSecretOperationFactory> {
    static constexpr const char* kNotConfigured =
        "Trying to decrypt the data-at-rest with a key from a Vault server "
        "but the system was not configured using Vault. Please remove the "
        "`--vaultRotateMasterKey` comand line option and the "
        "`security.vault.rotateMasterKey` configuration file parameter if any of them was "
        "provided. Then specify the `--vaultSecret` and the `--vaultSecretVersion` command "
        "line options or the `security.vault.secret` and the "
        "`security.vault.SecretVersion` configuration file parameters";
    static constexpr const char* kNotEqualKeyIds =
        "The provided (via the command line option or the configuration file) Vault "
        "secret identifier is not equal to that the system is already configured with. "
        "If it was intended to rotate the master key, please add the "
        "`--vaultRotateMasterKey` command line option or the "
        "`security.vault.rotateMasterKey` configuration file parameter. "
        "Otherwise, please omit `--vaultSecret` and `--vaultSecretVersion` "
        "command line options and the `security.vault.secretVersion` and "
        "`security.vault.secretVersion` configuration parameters. ";
};

template<>
struct Messages<KmipKeyOperationFactory> {
    static constexpr const char* kNotConfigured =
        "Trying to decrypt the data-at-rest with a key from a KMIP server "
        "but the system was not configured using KMIP. Please remove the "
        "`--kmipRotateMasterKey` comand line option and the "
        "`security.kmip.rotateMasterKey` configuration file parameter if any of them was "
        "provided. Then specify the `--kmipKeyIdentifer` command line option or the "
        "`security.kmip.keyIdentifier` configuration file parameter.";
    static constexpr const char* kNotEqualKeyIds =
        "The provided (via the command line option or the configuration file) KMIP "
        "keyIdentifier is not equal to that the system is already configured with. "
        "If it was intended to rotate the master key, please add the "
        "`--kmipRotateMasterKey` command line option or the "
        "`security.kmip.rotateMasterKey` configuration file parameter. "
        "Otherwise, please omit the `--kmipMasterKeyId` command line option and "
        "the `security.kmip.keyIdentifier` configuration parameter.";

};

constexpr const char* kRotationEqualKeyIdsMsg =
    "The master encryption key rotation is in effect but the provided (via the command line "
    "option or the configuration file) key identifier is equal to that the system "
    "is already configured with. ";

constexpr const char* kNotEqualSecretPathsMsg =
    "The provided (via the command line option or the configuration file) Vault "
    "secret path is not equal to that the system is already configured with. "
    "If it was intended to rotate the master key, please add the "
    "`--vaultRotateMasterKey` command line option or the "
    "`security.vault.rotateMasterKey` configuration file parameter. "
    "Otherwise, please omit `--vaultSecret` command line option and "
    "the `security.vault.secretVersion` configuration parameter.";

template <typename Id>
class ConfiguredKeyIdDispatcher : public KeyIdConstVisitor {
public:
    ConfiguredKeyIdDispatcher(const Id*& target) : _target(target) {}

private:
    void visit(const KeyFilePath& configured) override {
        invariant(false &&
                  "encryption key file path must not be serialized to the storage engine "
                  "metadata and thus must not appear in the configured key identifier");
    }
    void visit(const VaultSecretId& configured) override {
        _visit(_target, configured);
    }
    void visit(const KmipKeyId& configured) override {
        _visit(_target, configured);
    }

    static void _visit(const Id*& target, const Id& configured) {
        target = &configured;
    }
    static void _visit(const KmipKeyId*& target, const VaultSecretId& configured) {
        KeyErrorBuilder b(
            "Trying to decrypt the data-at-rest with the key from a KMIP server "
            "but the system was configured with a key from a Vault server. "
            "Please replace the `--vaultServerName` command line option with `--kmipServerName` "
            "or the `security.vault.serverName` configuration file parameter with "
            "`security.kmip.serverName`. Alternatively, if it was intended to migrate from "
            "Vault to KMIP, create a new empty database, specify `security.kmip.serverName` "
            "and migrate all the data to the new database.");
        throw b.error();
    }
    static void _visit(const VaultSecretId*& target, const KmipKeyId& configured) {
        KeyErrorBuilder b(
            "Trying to decrypt the data-at-rest with the key from a Vault server "
            "but the system was configured with a key from a KMIP server. "
            "Please replace the `--kmipServerName` command line option with `--vaultServerName` "
            "or the `security.kmip.serverName` configuration file parameter with "
            "`security.vault.serverName`. Alternatively, if it was intended to migrate from "
            "KMIP to Vault, create a new empty database, specify `security.vault.serverName` "
            "and migrate all the data to the new database.");
        throw b.error();
    }

    const Id*& _target;
};

template <typename Derived>
std::unique_ptr<ReadKey> CreateReadImpl<Derived>::_createProvidedRead() const {
    auto derived = static_cast<const Derived*>(this);
    if (derived->_provided) {
        return derived->_doCreateRead(*derived->_provided);
    }
    return nullptr;
}

template <typename Derived>
std::unique_ptr<ReadKey> CreateReadImpl<Derived>::_createRead(const KeyId* configured) const {
    auto derived = static_cast<const Derived*>(this);

    if (configured) {
        auto d = detail::ConfiguredKeyIdDispatcher(derived->_configured);
        configured->accept(d);
    }

    if (derived->_rotateMasterKey) {
        if (!derived->_configured) {
            throw KeyErrorBuilder(Messages<Derived>::kNotConfigured).error();
        }
        if (derived->_provided && *derived->_provided == *derived->_configured) {
            KeyErrorBuilder b(kRotationEqualKeyIdsMsg);
            b.append("configured", *derived->_configured);
            b.append("provided", *derived->_provided);
            throw b.error();
        }
        return derived->_doCreateRead(*derived->_configured);
    }

    if (derived->_configured) {
        if (derived->_provided && *derived->_provided != *derived->_configured) {
            KeyErrorBuilder b(Messages<Derived>::kNotEqualKeyIds);
            b.append("configured", *derived->_configured);
            b.append("provided", *derived->_provided);
            throw b.error();
        }
        if constexpr (std::is_same_v<Derived, VaultSecretOperationFactory>) {
            if (!derived->_providedSecretPath.empty() &&
                derived->_providedSecretPath != derived->_configured->path()) {
                KeyErrorBuilder b(kNotEqualSecretPathsMsg);
                b.append("configuredSecretPath", derived->_configured->path());
                b.append("providedSecretPath", derived->_providedSecretPath);
                throw b.error();
            }
        }
        return derived->_doCreateRead(*derived->_configured);
    }

    if (auto providedRead = derived->createProvidedRead(); providedRead) {
        return providedRead;
    }
    if constexpr (std::is_same_v<Derived, VaultSecretOperationFactory>) {
        if (!derived->_providedSecretPath.empty()) {
            // For Vault, use the latest key version (encoded with the special
            // value `0`), if `mongod` is about to read existing encrypted data
            // but there is no configured key identifier nor provided version.
            // That ensures `mongod` smooth upgrade from the older
            // versions, which always read the latest key version.
            return derived->_doCreateRead(VaultSecretId(derived->_providedSecretPath, 0));
        }
    }

    throw KeyErrorBuilder(Messages<Derived>::kNotConfigured).error();
}
}  // namespace detail

std::unique_ptr<ReadKey> VaultSecretOperationFactory::createProvidedRead() const {
    return _createProvidedRead();
}

std::unique_ptr<ReadKey> KmipKeyOperationFactory::createProvidedRead() const {
    return _createProvidedRead();
}

std::unique_ptr<ReadKey> VaultSecretOperationFactory::createRead(const KeyId* configured) const {
    return _createRead(configured);
}

std::unique_ptr<ReadKey> KmipKeyOperationFactory::createRead(const KeyId* configured) const {
    return _createRead(configured);
}

std::unique_ptr<SaveKey> VaultSecretOperationFactory::createSave(const KeyId* configured) const {
    if (!_providedSecretPath.empty()) {
        return _doCreateSave(_providedSecretPath);
    }

    if (configured) {
        auto d = detail::ConfiguredKeyIdDispatcher(_configured);
        configured->accept(d);
        if (_configured) {
            return _doCreateSave(_configured->path());
        }
    }

    KeyErrorBuilder b(
        "No Vault secret path is provided. Please specify either the `--vaultSecret` "
        "command line option or the `security.vault.secret` configuration file parameter.");
    throw b.error();
}
}  // namespace mongo::encryption
