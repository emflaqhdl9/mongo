/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/api_parameters.h"
#include "mongo/db/auth/authorization_checks.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/query/getmore_command_gen.h"
#include "mongo/db/stats/counters.h"
#include "mongo/s/query/cluster_cursor_manager.h"
#include "mongo/s/query/cluster_find.h"

namespace mongo {
namespace {

// getMore can run with any readConcern, because cursor-creating commands like find can run with any
// readConcern.  However, since getMore automatically uses the readConcern of the command that
// created the cursor, it is not appropriate to apply the default readConcern (just as
// client-specified readConcern isn't appropriate).
static const ReadConcernSupportResult kSupportsReadConcernResult{
    Status::OK(),
    {{ErrorCodes::InvalidOptions,
      "default read concern not permitted (getMore uses the cursor's read concern)"}}};

/**
 * Implements the getMore command on mongos. Retrieves more from an existing mongos cursor
 * corresponding to the cursor id passed from the application. In order to generate these results,
 * may issue getMore commands to remote nodes in one or more shards.
 */
class ClusterGetMoreCmd final : public Command {
public:
    ClusterGetMoreCmd() : Command("getMore") {}

    // Do not currently use apiVersions because clients are prohibited from calling
    // getMore with apiVersion.
    const std::set<std::string>& apiVersions() const {
        return kApiVersions1;
    }

    std::unique_ptr<CommandInvocation> parse(OperationContext* opCtx,
                                             const OpMsgRequest& opMsgRequest) override {
        return std::make_unique<Invocation>(this, opMsgRequest);
    }

    class Invocation final : public CommandInvocation {
    public:
        Invocation(Command* cmd, const OpMsgRequest& request)
            : CommandInvocation(cmd),
              _cmd(GetMoreCommandRequest::parse({"getMore"}, request.body)) {
            APIParameters::uassertNoApiParameters(request.body);
        }

    private:
        NamespaceString ns() const override {
            return NamespaceString(_cmd.getDbName(), _cmd.getCollection());
        }

        bool supportsWriteConcern() const override {
            return false;
        }

        ReadConcernSupportResult supportsReadConcern(repl::ReadConcernLevel level) const override {
            return kSupportsReadConcernResult;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassertStatusOK(auth::checkAuthForGetMore(AuthorizationSession::get(opCtx->getClient()),
                                                      ns(),
                                                      _cmd.getCommandParameter(),
                                                      _cmd.getTerm().is_initialized()));
        }

        void run(OperationContext* opCtx, rpc::ReplyBuilderInterface* reply) override {
            // Counted as a getMore, not as a command.
            globalOpCounters.gotGetMore();
            auto bob = reply->getBodyBuilder();
            auto response = uassertStatusOK(ClusterFind::runGetMore(opCtx, _cmd));
            response.addToBSON(CursorResponse::ResponseType::SubsequentResponse, &bob);

            if (getTestCommandsEnabled()) {
                validateResult(bob.asTempObj());
            }
        }

        void validateResult(const BSONObj& replyObj) {
            CursorGetMoreReply::parse({"CursorGetMoreReply"}, replyObj.removeField("ok"));
        }

        const GetMoreCommandRequest _cmd;
    };

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool maintenanceOk() const override {
        return false;
    }

    bool adminOnly() const override {
        return false;
    }

    /**
     * A getMore command increments the getMore counter, not the command counter.
     */
    bool shouldAffectCommandCounter() const override {
        return false;
    }

    std::string help() const override {
        return "retrieve more documents for a cursor id";
    }

    LogicalOp getLogicalOp() const override {
        return LogicalOp::opGetMore;
    }
} cmdGetMoreCluster;

}  // namespace
}  // namespace mongo
