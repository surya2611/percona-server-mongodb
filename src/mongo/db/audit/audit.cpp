/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*- */
// vim: ft=cpp:expandtab:ts=8:sw=4:softtabstop=4:

/*======
This file is part of Percona Server for MongoDB.

Copyright (C) 2018-present Percona and/or its affiliates. All rights reserved.

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

#ifdef PERCONA_AUDIT_ENABLED

#include <cstdio>
#include <iostream>
#include <string>

#include <syslog.h>

#include <boost/filesystem/path.hpp>
#include <boost/iostreams/device/file_descriptor.hpp>
#include <boost/iostreams/stream.hpp>
#include <boost/scoped_ptr.hpp>
#include <fmt/format.h>

#include "mongo/util/debug_util.h"
#include "mongo/util/net/socket_utils.h"

#include "mongo/base/init.h"
#include "mongo/bson/bson_field.h"
#include "mongo/db/audit.h"
#include "mongo/db/audit/audit_parameters_gen.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/matcher.h"
#include "mongo/db/namespace_string.h"
#include "mongo/logger/auditlog.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_util.h"
#include "mongo/platform/mutex.h"
#include "mongo/rpc/metadata/impersonated_user_metadata.h"
#include "mongo/util/concurrency/mutex.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/exit_code.h"
#include "mongo/util/net/sock.h"
#include "mongo/util/string_map.h"

#include "audit_options.h"

#define PERCONA_AUDIT_STUB {}

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


namespace mongo {

namespace audit {

    using namespace fmt::literals;

    // JsonStringFormat used by audit logs
    const JsonStringFormat auditJsonFormat = LegacyStrict;

    MONGO_COMPILER_NOINLINE void realexit( ExitCode rc ) {
#ifdef _COVERAGE
        // Need to make sure coverage data is properly flushed before exit.
        // It appears that ::_exit() does not do this.
        LOGV2(29013, "calling regular ::exit() so coverage data may flush...");
        ::exit( rc );
#else
        ::_exit( rc );
#endif
    }

    /** A system error code's error message. */
    inline static std::string errnoWithDescription(int e) {
        return errorMessage(systemError(e));
    }

    // Adapter
    class AuditLogFormatAdapter {
    public:
        virtual ~AuditLogFormatAdapter() {}
        virtual const char *data() const = 0;
        virtual unsigned size() const = 0;
    };

    // Writable interface for audit events
    class WritableAuditLog : public logv2::AuditLog {
    public:
        WritableAuditLog(const BSONObj &filter)
            : _matcher(filter.getOwned(), new ExpressionContext(nullptr, nullptr, NamespaceString())) {
        }
        virtual ~WritableAuditLog() {}

        void append(const BSONObj &obj, const bool affects_durable_state) {
            if (_matcher.matches(obj)) {
                appendMatched(obj, affects_durable_state);
            }
        }
        virtual Status rotate(bool rename,
                              StringData renameSuffix,
                              std::function<void(Status)> onMinorError) override {
            // No need to override this method if there is nothing to rotate
            // like it is for 'console' and 'syslog' destinations
            return Status::OK();
        }

        virtual void flush() {
            // No need to override this method if there is nothing to flush
            // like it is for 'console' and 'syslog' destinations

        }

        virtual void fsync() {
            // No need to override this method if there is nothing to fsync
            // like it is for 'console' and 'syslog' destinations
        }

    protected:
        virtual void appendMatched(const BSONObj &obj, const bool affects_durable_state) = 0;

    private:
        const Matcher _matcher;

    };

    // Writes audit events to a file
    class FileAuditLog : public WritableAuditLog {
        bool ioErrorShouldRetry(int errcode) {
            return (errcode == EAGAIN ||
                    errcode == EWOULDBLOCK ||
                    errcode == EINTR);
        }

        typedef boost::iostreams::stream<boost::iostreams::file_descriptor_sink> Sink;

    public:
        FileAuditLog(const std::string &file, const BSONObj &filter)
            : WritableAuditLog(filter),
              _file(new Sink),
              _fileName(file) {
            _file->open(file.c_str(), std::ios_base::out | std::ios_base::app | std::ios_base::binary);
        }

        virtual ~FileAuditLog() {
            if (_dirty) {
                flush_inlock();
                _dirty = false;
            }
        }

    protected:
        // Creates specific Adapter instance for FileAuditLog::append()
        // and passess ownership to caller
        virtual AuditLogFormatAdapter *createAdapter(const BSONObj &obj) const = 0;

        virtual void appendMatched(const BSONObj &obj, const bool affects_durable_state) override {
            boost::scoped_ptr<AuditLogFormatAdapter> adapter(createAdapter(obj));

            // mongo::File does not have an "atomic append" operation.
            // As such, with a rwlock we are vulnerable to a race
            // where we get the length of the file, then try to pwrite
            // at that offset.  If another write beats us to pwrite,
            // we'll overwrite that audit data when our write goes
            // through.
            //
            // Somewhere, we need a mutex around grabbing the file
            // offset and trying to write to it (even if this were in
            // the kernel, the synchronization is still there).  This
            // is a good enough place as any.
            //
            // We don't need the mutex around fsync, except to protect against concurrent
            // logRotate destroying our pointer.  Welp.
            stdx::lock_guard<SimpleMutex> lck(_mutex);

            _dirty = true;
            if (affects_durable_state)
                _fsync_pending = true;

            invariant(_membuf.write(adapter->data(), adapter->size()));
        }

        virtual Status rotate(bool rename,
                              StringData renameSuffix,
                              std::function<void(Status)> onMinorError) override {
            stdx::lock_guard<SimpleMutex> lck(_mutex);

            // Close the current file.
            _file.reset();

            if (rename) {
                // Rename the current file
                // Note: we append a timestamp to the file name.
                std::string s = _fileName + renameSuffix;
                int r = std::rename(_fileName.c_str(), s.c_str());
                if (r != 0) {
                    auto ec = lastSystemError();
                    if (onMinorError) {
                        onMinorError({ErrorCodes::FileRenameFailed,
                                      "Failed to rename {} to {}: {}"_format(
                                          _fileName, s, errorMessage(ec))});
                    }
                    LOGV2_ERROR(29016,
                                "Could not rotate audit log, but continuing normally "
                                "(error desc: {err_desc})",
                                "err_desc"_attr = errorMessage(ec));
                }
            }

            // Open a new file, with the same name as the original.
            _file.reset(new Sink);
            _file->open(_fileName.c_str());

            return Status::OK();
        }

        virtual void flush() override {
            stdx::lock_guard<SimpleMutex> lck(_mutex);

            if (_dirty) {
                flush_inlock();
                _dirty = false;
            }
        }

        virtual void fsync() override {
            stdx::lock_guard<SimpleMutex> lck(_mutex);

            if (_fsync_pending) {
                if (_dirty) {
                    flush_inlock();
                    _dirty = false;
                }

                fsync_inlock();
                _fsync_pending = false;
            }
        }

    private:
        std::ostringstream _membuf;
        boost::scoped_ptr<Sink> _file;
        const std::string _fileName;
        SimpleMutex _mutex;
        bool _dirty = false;
        bool _fsync_pending = false;

        void flush_inlock() {
            // If pwrite performs a partial write, we don't want to
            // muck about figuring out how much it did write (hard to
            // get out of the File abstraction) and then carefully
            // writing the rest.  Easier to calculate the position
            // first, then repeatedly write to that position if we
            // have to retry.
            auto pos = _file->tellp();
            auto data = _membuf.str();
            _membuf.str({});

            int writeRet;
            for (int retries = 10; retries > 0; --retries) {
                writeRet = 0;
                _file->seekp(pos);
                if (_file->write(data.c_str(), data.length()))
                    break;
                writeRet = errno;
                if (!ioErrorShouldRetry(writeRet)) {
                    LOGV2_ERROR(29017,
                                "Audit system cannot write {datalen} bytes to log file {file}. "
                                "Write failed with fatal error {err_desc}. "
                                "As audit cannot make progress, the server will now shut down.",
                                "datalen"_attr = data.length(),
                                "file"_attr = _fileName,
                                "err_desc"_attr = errnoWithDescription(writeRet));
                    realexit(EXIT_AUDIT_ERROR);
                }
                LOGV2_WARNING(29018,
                    "Audit system cannot write {datalen} bytes to log file {file}. "
                    "Write failed with retryable error {err_desc}. "
                    "Audit system will retry this write another {retries} times.",
                    "datalen"_attr = data.length(),
                    "file"_attr = _fileName,
                    "err_desc"_attr = errnoWithDescription(writeRet),
                    "retries"_attr = retries - 1);
                if (retries <= 7 && retries > 0) {
                    sleepmillis(1 << ((7 - retries) * 2));
                }
                _file->clear();
            }

            if (writeRet != 0) {
                LOGV2_ERROR(29019,
                    "Audit system cannot write {datalen} bytes to log file {file}. "
                    "Write failed with fatal error {err_desc}. "
                    "As audit cannot make progress, the server will now shut down.",
                    "datalen"_attr = data.length(),
                    "file"_attr = _fileName,
                    "err_desc"_attr = errnoWithDescription(writeRet));
                realexit(EXIT_AUDIT_ERROR);
            }

            _file->flush();
        }

        void fsync_inlock() {
            int fsyncRet;
            for (int retries = 10; retries > 0; --retries) {
                fsyncRet = ::fsync((*_file)->handle());
                if (fsyncRet == 0) {
                    break;
                } else if (!ioErrorShouldRetry(fsyncRet)) {
                    LOGV2_ERROR(29020,
                        "Audit system cannot fsync log file {file}. "
                        "Fsync failed with fatal error {err_desc}. "
                        "As audit cannot make progress, the server will now shut down.",
                        "file"_attr = _fileName,
                        "err_desc"_attr = errnoWithDescription(fsyncRet));
                    realexit(EXIT_AUDIT_ERROR);
                }
                LOGV2_WARNING(29021,
                    "Audit system cannot fsync log file {file}. "
                    "Fsync failed with retryable error {err_desc}. "
                    "Audit system will retry this fsync another {retries} times.",
                    "file"_attr = _fileName,
                    "err_desc"_attr = errnoWithDescription(fsyncRet),
                    "retries"_attr = retries - 1);
                if (retries <= 7 && retries > 0) {
                    sleepmillis(1 << ((7 - retries) * 2));
                }
            }

            if (fsyncRet != 0) {
                LOGV2_ERROR(29022,
                    "Audit system cannot fsync log file {file}. "
                    "Fsync failed with fatal error {err_desc}. "
                    "As audit cannot make progress, the server will now shut down.",
                    "file"_attr = _fileName,
                    "err_desc"_attr = errnoWithDescription(fsyncRet));
                realexit(EXIT_AUDIT_ERROR);
            }
        }
    };    

    // Writes audit events to a json file
    class JSONAuditLog: public FileAuditLog {

        class Adapter: public AuditLogFormatAdapter {
            const std::string str;

        public:
            Adapter(const BSONObj &obj)
                : str(obj.jsonString(auditJsonFormat) + '\n') {}

            virtual const char *data() const override {
                return str.c_str();
            }
            virtual unsigned size() const override {
                return str.size();
            }
        };

    protected:
        virtual AuditLogFormatAdapter *createAdapter(const BSONObj &obj) const override {
            return new Adapter(obj);
        }
    public:
        JSONAuditLog(const std::string &file, const BSONObj &filter)
            : FileAuditLog(file, filter) {}
    };

    // Writes audit events to a bson file
    class BSONAuditLog: public FileAuditLog {

        class Adapter: public AuditLogFormatAdapter {
            const BSONObj &obj;

        public:
            Adapter(const BSONObj &aobj)
                : obj(aobj) {}

            virtual const char *data() const override {
                return obj.objdata();
            }
            virtual unsigned size() const override {
                return obj.objsize();
            }
        };

    protected:
        virtual AuditLogFormatAdapter *createAdapter(const BSONObj &obj) const override {
            return new Adapter(obj);
        }

    public:
        BSONAuditLog(const std::string &file, const BSONObj &filter)
            : FileAuditLog(file, filter) {}
    };

    // Writes audit events to the console
    class ConsoleAuditLog : public WritableAuditLog {
    public:
        ConsoleAuditLog(const BSONObj &filter)
            : WritableAuditLog(filter) {
        }

    private:
        virtual void appendMatched(const BSONObj &obj, const bool affects_durable_state) override {
            std::cout << obj.jsonString(auditJsonFormat) << std::endl;
        }

    };

    // Writes audit events to the syslog
    class SyslogAuditLog : public WritableAuditLog {
    public:
        SyslogAuditLog(const BSONObj &filter)
            : WritableAuditLog(filter) {
        }

    private:
        virtual void appendMatched(const BSONObj &obj, const bool affects_durable_state) override {
            syslog(LOG_MAKEPRI(LOG_USER, LOG_INFO), "%s", obj.jsonString(auditJsonFormat).c_str());
        }

    };

    // A void audit log does not actually write any audit events. Instead, it
    // verifies that we can call jsonString() on the generatd bson obj and that
    // the result is non-empty. This is useful for sanity testing the audit bson
    // generation code even when auditing is not explicitly enabled in debug builds.
    class VoidAuditLog : public WritableAuditLog {
    public:
        VoidAuditLog(const BSONObj &filter)
            : WritableAuditLog(filter) {
        }

    protected:
        void appendMatched(const BSONObj &obj, const bool affects_durable_state) override {
            verify(!obj.jsonString(auditJsonFormat).empty());
        }

    };

    static std::shared_ptr<WritableAuditLog> _auditLog;

    static void _setGlobalAuditLog(WritableAuditLog *log) {
        // Must be the last line because this function is also used
        // for cleanup (log can be nullptr)
        // Otherwise race condition exists during cleanup.
        _auditLog.reset(log);
    }

    static bool _auditEnabledOnCommandLine() {
        return auditOptions.destination != "";
    }

    Status initialize() {
        if (!_auditEnabledOnCommandLine()) {
            // Write audit events into the void for debug builds, so we get
            // coverage on the code that generates audit log objects.
            if (kDebugBuild) {
                LOGV2(29014, "Initializing dev null audit...");
                _setGlobalAuditLog(new VoidAuditLog(fromjson(auditOptions.filter)));
            }
            return Status::OK();
        }

        LOGV2(29015, "Initializing audit...");
        const BSONObj filter = fromjson(auditOptions.filter);
        if (auditOptions.destination == "console")
            _setGlobalAuditLog(new ConsoleAuditLog(filter));
        else if (auditOptions.destination == "syslog")
            _setGlobalAuditLog(new SyslogAuditLog(filter));
        // "file" destination
        else if (auditOptions.format == "BSON")
            _setGlobalAuditLog(new BSONAuditLog(auditOptions.path, filter));
        else
            _setGlobalAuditLog(new JSONAuditLog(auditOptions.path, filter));
        return Status::OK();
    }

    MONGO_INITIALIZER_WITH_PREREQUISITES(AuditInit,
                                         ("default", "PathlessOperatorMap", "MatchExpressionParser"))
        (InitializerContext *context) {
        // Sets the audit log in the general logging framework which
        // will rotate() the audit log when the server log rotates.
        logv2::addLogRotator(
            logv2::kAuditLogTag,
            [](bool renameFiles, StringData suffix, std::function<void(Status)> onMinorError) {
                if (_auditLog) {
                    return _auditLog->rotate(renameFiles, suffix, onMinorError);
                }
                return Status::OK();
            });
        uassertStatusOK(initialize());
    }

///////////////////////// audit.h functions ////////////////////////////

    namespace AuditFields {
        // Common fields
        BSONField<StringData> type("atype");
        BSONField<Date_t> timestamp("ts");
        BSONField<BSONObj> local("local");
        BSONField<BSONObj> remote("remote");
        BSONField<BSONObj> param("param");
        BSONField<int> result("result");
    }

    // This exists because NamespaceString::toString() prints "admin."
    // when dbname == "admin" and coll == "", which isn't so great.
    static std::string nssToString(const NamespaceString &nss) {
        std::stringstream ss;
        if (!nss.db().empty()) {
            ss << nss.db();
        }

        if (!nss.coll().empty()) {
            ss << '.' << nss.coll();
        }

        return ss.str();
    }

    static void appendRoles(BSONObjBuilder& builder, RoleNameIterator it) {
        BSONArrayBuilder rolebuilder(builder.subarrayStart("roles"));
        for (; it.more(); it.next()) {
            BSONObjBuilder r(rolebuilder.subobjStart());
            r.append("role", it->getRole());
            r.append("db", it->getDB());
            r.doneFast();
        }
        rolebuilder.doneFast();
    }

    static void appendRoles(BSONObjBuilder& builder, const std::vector<RoleName>& roles) {
        appendRoles(builder, makeRoleNameIterator(roles.begin(), roles.end()));
    }

    static std::string getIpByHost(const std::string& host) {
        if (host.empty()) {
            return {};
        }

        static StringMap<std::string> hostToIpCache;
        static auto cacheMutex = MONGO_MAKE_LATCH("audit::getIpByHost::cacheMutex");

        std::string ip;
        {
            stdx::lock_guard<Latch> lk(cacheMutex);
            ip = hostToIpCache[host];
        }
        if (ip.empty()) {
            ip = hostbyname(host.c_str());
            stdx::lock_guard<Latch> lk(cacheMutex);
            hostToIpCache[host] = ip;
        }
        return ip;
    }

    static void appendCommonInfo(BSONObjBuilder &builder,
                                 StringData atype,
                                 Client* client) {
        builder << AuditFields::type(atype);
        builder << AuditFields::timestamp(jsTime());
        builder << AuditFields::local(
            BSON("ip" << getIpByHost(getHostNameCached()) << "port" << serverGlobalParams.port));
        if (client->hasRemote()) {
            const HostAndPort hp = client->getRemote();
            builder << AuditFields::remote(
                BSON("ip" << getIpByHost(hp.host()) << "port" << hp.port()));
        } else {
            // It's not 100% clear that an empty obj here actually makes sense..
            builder << AuditFields::remote(BSONObj());
        }
        if (AuthorizationSession::exists(client)) {
            // Build the users array, which consists of (user, db) pairs
            AuthorizationSession *session = AuthorizationSession::get(client);
            BSONArrayBuilder users(builder.subarrayStart("users"));
            for (UserNameIterator it = session->getAuthenticatedUserNames(); it.more(); it.next()) {
                BSONObjBuilder user(users.subobjStart());
                user.append("user", it->getUser());
                user.append("db", it->getDB());
                user.doneFast();
            }
            users.doneFast();
            appendRoles(builder, session->getAuthenticatedRoleNames());
        } else {
            // It's not 100% clear that an empty obj here actually makes sense..
            builder << "users" << BSONObj();
        }
    }

    static void appendPrivileges(BSONObjBuilder &builder, const PrivilegeVector& privileges) {
        BSONArrayBuilder privbuilder(builder.subarrayStart("privileges"));
        for (PrivilegeVector::const_iterator it = privileges.begin(); it != privileges.end(); ++it) {
            privbuilder.append(it->toBSON());
        }
        privbuilder.doneFast();
    }


    static void _auditEvent(Client* client,
                            StringData atype,
                            const BSONObj& params,
                            ErrorCodes::Error result = ErrorCodes::OK,
                            const bool affects_durable_state = true) {
        BSONObjBuilder builder;
        appendCommonInfo(builder, atype, client);
        builder << AuditFields::param(params);
        builder << AuditFields::result(static_cast<int>(result));
        _auditLog->append(builder.done(), affects_durable_state);
    }

    static void _auditAuthz(Client* client,
                            const NamespaceString& nss,
                                 StringData command,
                                 const BSONObj& args,
                                 ErrorCodes::Error result) {
        if ((result != ErrorCodes::OK) || auditAuthorizationSuccess.load()) {
            std::string ns = nssToString(nss);
            const BSONObj params = !ns.empty() ?
                BSON("command" << command << "ns" << ns << "args" << args) :
                BSON("command" << command << "args" << args);
            _auditEvent(client, "authCheck", params, result, false);
        }
    }

    static void _auditSystemUsers(Client* client,
                                  const NamespaceString& ns,
                                  StringData atype,
                                  const BSONObj& params,
                                  ErrorCodes::Error result) {
        if ((result == ErrorCodes::OK) && (ns.coll() == "system.users")) {
            _auditEvent(client, atype, params);
        }

    }

    ImpersonatedClientAttrs::ImpersonatedClientAttrs(Client* client) {
        auto optAttrs = rpc::getImpersonatedUserMetadata(client->getOperationContext());
        if (optAttrs) {
            userNames = optAttrs->getUsers();
            roleNames = optAttrs->getRoles();
        }
    }

    void rotateAuditLog() {}

    void logClientMetadata(Client* client) {
        if (!_auditLog) {
            return;
        }

        _auditEvent(client, "clientMetadata", BSONObj{}, ErrorCodes::OK, false);
    }
    void logAuthentication(Client* client, const AuthenticateEvent& event) {
        if (!_auditLog) {
            return;
        }

        const BSONObj params = BSON("user" << event.getUser() << "db" << event.getDatabase()
                                           << "mechanism" << event.getMechanism());
        _auditEvent(client, "authenticate", params, event.getResult(), false);
    }

    void logCommandAuthzCheck(Client* client,
                              const OpMsgRequest& cmdObj,
                              const CommandInterface& command,
                              ErrorCodes::Error result) {
        if (!_auditLog) {
            return;
        }

        _auditAuthz(client, command.ns(), cmdObj.body.firstElement().fieldName(), cmdObj.body, result);
    }


    void logDeleteAuthzCheck(
            Client* client,
            const NamespaceString& ns,
            const BSONObj& pattern,
            ErrorCodes::Error result) {
        if (!_auditLog) {
            return;
        }

        _auditAuthz(client, ns, "delete", BSON("pattern" << pattern), result);
        _auditSystemUsers(client, ns, "dropUser",
                          BSON("db" << ns.db() << "pattern" << pattern), result);
    }

    void logGetMoreAuthzCheck(
            Client* client,
            const NamespaceString& ns,
            long long cursorId,
            ErrorCodes::Error result) {
        if (!_auditLog) {
            return;
        }

        _auditAuthz(client, ns, "getMore", BSON("cursorId" << cursorId), result);
    }

    void logInsertAuthzCheck(
            Client* client,
            const NamespaceString& ns,
            const BSONObj& insertedObj,
            ErrorCodes::Error result) {
        if (!_auditLog) {
            return;
        }

        _auditAuthz(client, ns, "insert", BSON("obj" << insertedObj), result);
        _auditSystemUsers(client, ns, "createUser",
                          BSON("db" << ns.db() << "userObj" << insertedObj), result);
    }

    void logKillCursorsAuthzCheck(
            Client* client,
            const NamespaceString& ns,
            long long cursorId,
            ErrorCodes::Error result) {
        if (!_auditLog) {
            return;
        }

        _auditAuthz(client, ns, "killCursors", BSON("cursorId" << cursorId), result);
    }

    void logQueryAuthzCheck(
            Client* client,
            const NamespaceString& ns,
            const BSONObj& query,
            ErrorCodes::Error result) {
        if (!_auditLog) {
            return;
        }

        _auditAuthz(client, ns, "query", BSON("query" << query), result);
    }

    void logUpdateAuthzCheck(
            Client* client,
            const NamespaceString& ns,
            const BSONObj& query,
            const write_ops::UpdateModification& update,
            bool isUpsert,
            bool isMulti,
            ErrorCodes::Error result) {
        if (!_auditLog) {
            return;
        }

        using UpdateType = write_ops::UpdateModification::Type;
        const auto updateType = update.type();
        invariant(updateType == UpdateType::kReplacement || updateType == UpdateType::kModifier);
        const auto& updateObj = updateType == UpdateType::kReplacement
            ? update.getUpdateReplacement()
            : update.getUpdateModifier();
        {
            const BSONObj args = BSON("pattern" << query << "updateObj" << updateObj << "upsert"
                                                << isUpsert << "multi" << isMulti);
            _auditAuthz(client, ns, "update", args, result);
        }
        {
            const BSONObj params =
                BSON("db" << ns.db() << "pattern" << query << "updateObj" << updateObj << "upsert"
                          << isUpsert << "multi" << isMulti);
            _auditSystemUsers(client, ns, "updateUser", params, result);
        }
    }

    void logReplSetReconfig(Client* client,
                            const BSONObj* oldConfig,
                            const BSONObj* newConfig) {
        if (!_auditLog) {
            return;
        }

        const BSONObj params = BSON("old" << *oldConfig << "new" << *newConfig);
        _auditEvent(client, "replSetReconfig", params);
    }

    void logApplicationMessage(Client* client,
                               StringData msg) {
        if (!_auditLog) {
            return;
        }

        const BSONObj params = BSON("msg" << msg);
        _auditEvent(client, "applicationMessage", params, ErrorCodes::OK, false);
    }

    void logStartupOptions(Client* client, const BSONObj& startupOptions) {
        if (!_auditLog) {
            return;
        }

        _auditEvent(client, "startupOptions", startupOptions, ErrorCodes::OK, false);
    }

    void logShutdown(Client* client) {
        if (!_auditLog) {
            return;
        }

        const BSONObj params = BSONObj();
        _auditEvent(client, "shutdown", params);

        // This is always the last event
        // Destroy audit log here
        _setGlobalAuditLog(nullptr);
    }

    void logLogout(Client* client,
                   StringData reason,
                   const BSONArray& initialUsers,
                   const BSONArray& updatedUsers) {
        if (!_auditLog) {
            return;
        }

        const BSONObj params = BSON("reason" << reason << "initialUsers" << initialUsers
                                             << "updatedUsers" << updatedUsers);
        _auditEvent(client, "logout", params, ErrorCodes::OK, false);
    }

    void logCreateIndex(Client* client,
                        const BSONObj* indexSpec,
                        StringData indexname,
                        const NamespaceString& nsname,
                        StringData indexBuildState,
                        ErrorCodes::Error result) {
        if (!_auditLog) {
            return;
        }

        BSONObjBuilder params;
        params.append("ns", nsname.ns());
        params.append("indexName", indexname);
        params.append("indexSpec", *indexSpec);
        params.append("indexBuildState", indexBuildState);
        _auditEvent(client, "createIndex", params.done(), result);
    }

    void logCreateCollection(Client* client, const NamespaceString& nsname) {
        if (!_auditLog) {
            return;
        }

        const BSONObj params = BSON("ns" << nsname.ns());
        _auditEvent(client, "createCollection", params);
    }

    void logCreateView(Client* client,
                       const NamespaceString& nsname,
                       StringData viewOn,
                       BSONArray pipeline,
                       ErrorCodes::Error code) {
        if (!_auditLog) {
            return;
        }

        const BSONObj params =
            BSON("ns" << nsname.ns() << "viewOn" << viewOn << "pipeline" << pipeline);
        _auditEvent(client, "createView", params, code);
    }

    void logImportCollection(Client* client, const NamespaceString& nsname) {
        if (!_auditLog) {
            return;
        }

        const BSONObj params = BSON("ns" << nsname.ns());
        _auditEvent(client, "importCollection", params);
    }

    void logCreateDatabase(Client* client,
                           StringData nsname) {
        if (!_auditLog) {
            return;
        }

        const BSONObj params = BSON("ns" << nsname);
        _auditEvent(client, "createDatabase", params);
    }

    void logDropIndex(Client* client, StringData indexname, const NamespaceString& nsname) {
        if (!_auditLog) {
            return;
        }

        const BSONObj params = BSON("ns" << nsname.ns() << "indexName" << indexname);
        _auditEvent(client, "dropIndex", params);
    }

    void logDropCollection(Client* client, const NamespaceString& nsname) {
        if (!_auditLog) {
            return;
        }

        const BSONObj params = BSON("ns" << nsname.ns());
        _auditEvent(client, "dropCollection", params);
    }

    void logDropView(Client* client,
                     const NamespaceString& nsname,
                     StringData viewOn,
                     const std::vector<BSONObj>& pipeline,
                     ErrorCodes::Error code) {
        if (!_auditLog) {
            return;
        }

        BSONObjBuilder params;
        params.append("ns", nsname.ns());
        params.append("viewOn", viewOn);
        params.append("pipeline", pipeline);
        _auditEvent(client, "dropView", params.done(), code);
    }

    void logDropDatabase(Client* client,
                         StringData nsname) {
        if (!_auditLog) {
            return;
        }

        const BSONObj params = BSON("ns" << nsname);
        _auditEvent(client, "dropDatabase", params);
    }

    void logRenameCollection(Client* client,
                             const NamespaceString& source,
                             const NamespaceString& target) {
        if (!_auditLog) {
            return;
        }

        const BSONObj params = BSON("old" << nssToString(source) << "new" << nssToString(target));
        _auditEvent(client, "renameCollection", params);
    }

    void logEnableSharding(Client* client,
                           StringData nsname) {
        if (!_auditLog) {
            return;
        }

        const BSONObj params = BSON("ns" << nsname);
        _auditEvent(client, "enableSharding", params);
    }

    void logAddShard(Client* client,
                     StringData name,
                     const std::string& servers,
                     long long maxsize) {
        if (!_auditLog) {
            return;
        }

        const BSONObj params= BSON("shard" << name <<
                                   "connectionString" << servers <<
                                   "maxSize" << maxsize);
        _auditEvent(client, "addShard", params);
    }

    void logRemoveShard(Client* client,
                        StringData shardname) {
        if (!_auditLog) {
            return;
        }

        const BSONObj params = BSON("shard" << shardname);
        _auditEvent(client, "removeShard", params);
    }

    void logShardCollection(Client* client,
                            StringData ns,
                            const BSONObj& keyPattern,
                            bool unique) {
        if (!_auditLog) {
            return;
        }

        const BSONObj params = BSON("ns" << ns <<
                                    "key" << keyPattern <<
                                    "options" << BSON("unique" << unique));
        _auditEvent(client, "shardCollection", params);
    }

    void logCreateUser(Client* client,
                       const UserName& username,
                       bool password,
                       const BSONObj* customData,
                       const std::vector<RoleName>& roles,
                       const boost::optional<BSONArray>& restrictions) {
        if (!_auditLog) {
            return;
        }

        BSONObjBuilder params;
        params << "user" << username.getUser()
               << "db" << username.getDB() 
               << "password" << password 
               << "customData" << (customData ? *customData : BSONObj());
        appendRoles(params, roles);
        _auditEvent(client, "createUser", params.done());
    }

    void logDropUser(Client* client,
                     const UserName& username) {
        if (!_auditLog) {
            return;
        }

        const BSONObj params = BSON("user" << username.getUser() <<
                                    "db" << username.getDB());
        _auditEvent(client, "dropUser", params);
    }

    void logDropAllUsersFromDatabase(Client* client,
                                     StringData dbname) {
        if (!_auditLog) {
            return;
        }

        _auditEvent(client, "dropAllUsers", BSON("db" << dbname));
    }

    void logUpdateUser(Client* client,
                       const UserName& username,
                       bool password,
                       const BSONObj* customData,
                       const std::vector<RoleName>* roles,
                       const boost::optional<BSONArray>& restrictions) {
        if (!_auditLog) {
            return;
        }

        BSONObjBuilder params;
        params << "user" << username.getUser()
               << "db" << username.getDB() 
               << "password" << password 
               << "customData" << (customData ? *customData : BSONObj());
        if (roles) {
            appendRoles(params, *roles);
        }

        _auditEvent(client, "updateUser", params.done());
    }

    void logGrantRolesToUser(Client* client,
                             const UserName& username,
                             const std::vector<RoleName>& roles) {
        if (!_auditLog) {
            return;
        }

        BSONObjBuilder params;
        params << "user" << username.getUser()
               << "db" << username.getDB();
        appendRoles(params, roles);
        _auditEvent(client, "grantRolesToUser", params.done());
    }

    void logRevokeRolesFromUser(Client* client,
                                const UserName& username,
                                const std::vector<RoleName>& roles) {
        if (!_auditLog) {
            return;
        }

        BSONObjBuilder params;
        params << "user" << username.getUser()
               << "db" << username.getDB();
        appendRoles(params, roles);
        _auditEvent(client, "revokeRolesFromUser", params.done());
    }

    void logCreateRole(Client* client,
                       const RoleName& role,
                       const std::vector<RoleName>& roles,
                       const PrivilegeVector& privileges,
                       const boost::optional<BSONArray>& restrictions) {
        if (!_auditLog) {
            return;
        }

        BSONObjBuilder params;
        params << "role" << role.getRole()
               << "db" << role.getDB();
        appendRoles(params, roles);
        appendPrivileges(params, privileges);
        _auditEvent(client, "createRole", params.done());
    }

    void logUpdateRole(Client* client,
                       const RoleName& role,
                       const std::vector<RoleName>* roles,
                       const PrivilegeVector* privileges,
                       const boost::optional<BSONArray>& restrictions) {
        if (!_auditLog) {
            return;
        }

        BSONObjBuilder params;
        params << "role" << role.getRole()
               << "db" << role.getDB();
        if (roles) {
            appendRoles(params, *roles);
        }
        if (privileges) {
            appendPrivileges(params, *privileges);
        }
        _auditEvent(client, "updateRole", params.done());
    }

    void logDropRole(Client* client,
                     const RoleName& role) {
        if (!_auditLog) {
            return;
        }

        const BSONObj params = BSON("role" << role.getRole() <<
                                    "db" << role.getDB());
        _auditEvent(client, "dropRole", params);
    }

    void logDropAllRolesFromDatabase(Client* client,
                                     StringData dbname) {
        if (!_auditLog) {
            return;
        }

        _auditEvent(client, "dropAllRoles", BSON("db" << dbname));
    }

    void logGrantRolesToRole(Client* client,
                             const RoleName& role,
                             const std::vector<RoleName>& roles) {
        if (!_auditLog) {
            return;
        }

        BSONObjBuilder params;
        params << "role" << role.getRole()
               << "db" << role.getDB();
        appendRoles(params, roles);
        _auditEvent(client, "grantRolesToRole", params.done());
    }

    void logRevokeRolesFromRole(Client* client,
                                const RoleName& role,
                                const std::vector<RoleName>& roles) {
        if (!_auditLog) {
            return;
        }

        BSONObjBuilder params;
        params << "role" << role.getRole()
               << "db" << role.getDB();
        appendRoles(params, roles);
        _auditEvent(client, "revokeRolesFromRole", params.done());
    }

    void logGrantPrivilegesToRole(Client* client,
                                  const RoleName& role,
                                  const PrivilegeVector& privileges) {
        if (!_auditLog) {
            return;
        }

        BSONObjBuilder params;
        params << "role" << role.getRole()
               << "db" << role.getDB();
        appendPrivileges(params, privileges);
        _auditEvent(client, "grantPrivilegesToRole", params.done());
    }

    void logRevokePrivilegesFromRole(Client* client,
                                     const RoleName& role,
                                     const PrivilegeVector& privileges) {
        if (!_auditLog) {
            return;
        }

        BSONObjBuilder params;
        params << "role" << role.getRole()
               << "db" << role.getDB();
        appendPrivileges(params, privileges);
        _auditEvent(client, "revokePrivilegesFromRole", params.done());
    }

    void logRefineCollectionShardKey(Client* client,
                                     StringData ns,
                                     const BSONObj& keyPattern) {
        if (!_auditLog) {
            return;
        }

        const BSONObj params = BSON("ns" << ns <<
                                    "key" << keyPattern);
        _auditEvent(client, "refineCollectionShardKey", params);
    }

    void logInsertOperation(Client* client, const NamespaceString& nss, const BSONObj& doc) {
        if (!_auditLog) {
            return;
        }

        const BSONObj params = BSON("ns" << nssToString(nss) << "doc" << doc);
        _auditEvent(client, "insertOperation", params);
    }

    void logUpdateOperation(Client* client, const NamespaceString& nss, const BSONObj& doc) {
        if (!_auditLog) {
            return;
        }

        const BSONObj params = BSON("ns" << nssToString(nss) << "doc" << doc);
        _auditEvent(client, "updateOperation", params);
    }

    void logRemoveOperation(Client* client, const NamespaceString& nss, const BSONObj& doc) {
        if (!_auditLog) {
            return;
        }

        const BSONObj params = BSON("ns" << nssToString(nss) << "doc" << doc);
        _auditEvent(client, "removeOperation", params);
    }

    void writeImpersonatedUsersToMetadata(OperationContext* txn,
                                          BSONObjBuilder* metadata) PERCONA_AUDIT_STUB

    void parseAndRemoveImpersonatedUsersField(
            BSONObj cmdObj,
            AuthorizationSession* authSession,
            std::vector<UserName>* parsedUserNames,
            bool* fieldIsPresent) PERCONA_AUDIT_STUB

    void parseAndRemoveImpersonatedRolesField(
            BSONObj cmdObj,
            AuthorizationSession* authSession,
            std::vector<RoleName>* parsedRoleNames,
            bool* fieldIsPresent) PERCONA_AUDIT_STUB


    void flushAuditLog() {
        if (!_auditLog) {
            return;
        }

        _auditLog->flush();
    }

    void fsyncAuditLog() {
        if (!_auditLog) {
            return;
        }

        _auditLog->fsync();
    }

}  // namespace audit
}  // namespace mongo

#endif  // PERCONA_AUDIT_ENABLED
