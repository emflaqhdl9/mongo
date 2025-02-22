/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/rename_collection.h"
#include "mongo/db/commands.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/rename_collection_coordinator.h"
#include "mongo/db/s/rename_collection_coordinator_document_gen.h"
#include "mongo/db/s/sharding_ddl_coordinator_service.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/logv2/log.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"
#include "mongo/s/sharded_collections_ddl_parameters_gen.h"

namespace mongo {
namespace {

bool isCollectionSharded(OperationContext* opCtx, const NamespaceString& nss) {
    AutoGetCollectionForRead lock(opCtx, nss);
    return opCtx->writesAreReplicated() &&
        CollectionShardingState::get(opCtx, nss)->getCollectionDescription(opCtx).isSharded();
}

bool renameIsAllowedOnNS(const NamespaceString& nss) {
    if (nss.isSystem()) {
        return nss.isLegalClientSystemNS(serverGlobalParams.featureCompatibility);
    }

    return !nss.isOnInternalDb();
}

RenameCollectionResponse renameCollectionLegacy(OperationContext* opCtx,
                                                const ShardsvrRenameCollection& request,
                                                const NamespaceString& fromNss) {
    const auto& toNss = request.getTo();

    const auto fromDB = uassertStatusOK(
        Grid::get(opCtx)->catalogCache()->getDatabaseWithRefresh(opCtx, fromNss.db()));

    const auto toDB = uassertStatusOK(
        Grid::get(opCtx)->catalogCache()->getDatabaseWithRefresh(opCtx, toNss.db()));

    uassert(13137,
            "Source and destination collections must be on same shard",
            fromDB.primaryId() == toDB.primaryId());

    // Make sure that source and target collection are not sharded
    uassert(ErrorCodes::IllegalOperation,
            str::stream() << "source namespace '" << fromNss << "' must not be sharded",
            !isCollectionSharded(opCtx, fromNss));
    uassert(ErrorCodes::IllegalOperation,
            str::stream() << "cannot rename to sharded collection '" << toNss << "'",
            !isCollectionSharded(opCtx, toNss));

    RenameCollectionOptions options{request.getDropTarget(), request.getStayTemp()};
    validateAndRunRenameCollection(opCtx, fromNss, toNss, options);

    return RenameCollectionResponse(ChunkVersion::UNSHARDED());
}

class ShardsvrRenameCollectionCommand final : public TypedCommand<ShardsvrRenameCollectionCommand> {
public:
    using Request = ShardsvrRenameCollection;
    using Response = RenameCollectionResponse;

    std::string help() const override {
        return "Internal command. Do not call directly. Renames a collection.";
    }

    bool adminOnly() const override {
        return false;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        Response typedRun(OperationContext* opCtx) {
            const auto& req = request();
            const auto& fromNss = ns();
            const auto& toNss = req.getTo();

            auto const shardingState = ShardingState::get(opCtx);
            uassertStatusOK(shardingState->canAcceptShardedCommands());

            bool useNewPath = feature_flags::gShardingFullDDLSupport.isEnabled(
                serverGlobalParams.featureCompatibility);

            if (!useNewPath) {
                return renameCollectionLegacy(opCtx, req, fromNss);
            }

            uassert(ErrorCodes::InvalidOptions,
                    str::stream() << Request::kCommandName
                                  << " must be called with majority writeConcern, got "
                                  << opCtx->getWriteConcern().wMode,
                    opCtx->getWriteConcern().wMode == WriteConcernOptions::kMajority);

            uassert(ErrorCodes::CommandFailed,
                    "Source and destination collections must be on the same database.",
                    fromNss.db() == toNss.db());

            uassert(ErrorCodes::InvalidNamespace,
                    str::stream() << "Can't rename from internal namespace: " << fromNss,
                    renameIsAllowedOnNS(fromNss));

            uassert(ErrorCodes::InvalidNamespace,
                    str::stream() << "Can't rename to internal namespace: " << toNss,
                    renameIsAllowedOnNS(toNss));

            auto coordinatorDoc = RenameCollectionCoordinatorDocument();
            coordinatorDoc.setRenameCollectionRequest(req.getRenameCollectionRequest());
            coordinatorDoc.setShardingDDLCoordinatorMetadata(
                {{fromNss, DDLCoordinatorTypeEnum::kRenameCollection}});
            auto service = ShardingDDLCoordinatorService::getService(opCtx);
            auto renameCollectionCoordinator = checked_pointer_cast<RenameCollectionCoordinator>(
                service->getOrCreateInstance(opCtx, coordinatorDoc.toBSON()));
            return renameCollectionCoordinator->getResponse(opCtx);
        }

    private:
        NamespaceString ns() const override {
            return request().getNamespace();
        }

        bool supportsWriteConcern() const override {
            return true;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                           ActionType::internal));
        }
    };

} shardsvrRenameCollectionCommand;

}  // namespace
}  // namespace mongo
