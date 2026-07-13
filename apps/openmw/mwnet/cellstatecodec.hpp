#ifndef OPENMW_MWNET_CELLSTATECODEC_H
#define OPENMW_MWNET_CELLSTATECODEC_H

#include <string>
#include <vector>

namespace MWWorld
{
    class CellStore;
}

namespace MWNet
{
    /// Serialize one cell's current state to an opaque blob: a minimal save header followed by
    /// exactly the REC_CSTA record a save writes for the cell (cell state incl. respawn timing,
    /// and every changed/runtime reference with its RefNum verbatim) — minus fog of war (per-player
    /// exploration, not shared world state) and minus refs the live replication channels own
    /// (summons; avatars never sit in cell ref lists). The host serves this to a client loading
    /// the cell, so the client's copy matches the host's without per-category re-derivation.
    /// Ensures the CellStore is loaded (ref lists only — no scene needed).
    ///
    /// contentFiles is the load order the records are written against; identical content on both
    /// ends is an existing session invariant (enforced at login).
    std::string serializeCellState(MWWorld::CellStore& cell, const std::vector<std::string>& contentFiles);

    /// Apply a received cell blob through the same path a save-load uses (the REC_CSTA branch of
    /// WorldModel::readRecord: loadState + readReferences over the content-loaded cell). Content
    /// refs are overwritten in place; runtime refs are added to the cell's ref list — the caller
    /// is responsible for scene attachment (World::attachCellStateRefs) and for clearing stale
    /// host-origin runtime refs first (re-application would duplicate them), and should hold a
    /// Replicator::RemoteApplyScope so the applied changes are not re-reported. Returns false on
    /// malformed input (cleanly, hostile data cannot crash the reader).
    bool applyCellState(const std::string& blob);
}

#endif
