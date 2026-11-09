// RenderWare plugin data in streams, module batkbin (see rw/core/plugin.h).
#include "rw/core/binary.h"
#include "rw/core/error.h"
#include "rw/core/plugin.h"

namespace {

constexpr RwUInt32 kChunkHeaderSize = 12;
constexpr RwUInt32 kWriteVersion = rwLIBRARYCURRENTVERSION;   // what the game writes: 3.7.0.2, build 0x37
constexpr RwUInt32 kWriteBuild = 0x37;
constexpr RwInt32 E_RW_BADVERSION = static_cast<RwInt32>(0x80000004u);

RwPluginRegEntry* find(const RwPluginRegistry* reg, RwUInt32 pluginID) {
    RwPluginRegEntry* entry = reg->firstRegEntry;
    while (entry != nullptr && entry->pluginID != pluginID) entry = entry->nextRegEntry;
    return entry;
}

}  // namespace

RwInt32 _rwPluginRegistryAddPluginStream(RwPluginRegistry* reg, RwUInt32 pluginID,
                                         RwPluginDataChunkReadCallBack readCB, RwPluginDataChunkWriteCallBack writeCB,
                                         RwPluginDataChunkGetSizeCallBack getSizeCB) {
    RwPluginRegEntry* entry = find(reg, pluginID);
    if (entry == nullptr) return -1;
    entry->readCB = readCB;
    entry->writeCB = writeCB;
    entry->getSizeCB = getSizeCB;
    return entry->offset;
}

RwInt32 _rwPluginRegistryAddPlgnStrmlwysCB(RwPluginRegistry* reg, RwUInt32 pluginID,
                                           RwPluginDataChunkAlwaysCallBack alwaysCB) {
    RwPluginRegEntry* entry = find(reg, pluginID);
    if (entry == nullptr) return -1;
    entry->alwaysCB = alwaysCB;
    return entry->offset;
}

RwInt32 _rwPluginRegistryAddPlgnStrmRightsCB(RwPluginRegistry* reg, RwUInt32 pluginID,
                                             RwPluginDataChunkRightsCallBack rightsCB) {
    RwPluginRegEntry* entry = find(reg, pluginID);
    if (entry == nullptr) return -1;
    entry->rightsCB = rightsCB;
    return entry->offset;
}

const RwPluginRegistry* _rwPluginRegistryReadDataChunks(const RwPluginRegistry* reg, RwStream* stream, void* object) {
    RwUInt32 length = 0, version = 0;
    if (!RwStreamFindChunk(stream, rwID_EXTENSION, &length, &version)) return nullptr;
    if (version < rwLIBRARYBASEVERSION || version > rwLIBRARYCURRENTVERSION) {
        RwError error{rwID_COREPLUGIN, _rwerror(E_RW_BADVERSION)};
        RwErrorSet(&error);
        return nullptr;
    }
    while (length != 0) {
        RwUInt32 type = 0, chunkLength = 0;
        if (!_rwStreamReadChunkHeader(stream, &type, &chunkLength, nullptr, nullptr)) return nullptr;
        const RwPluginRegEntry* entry = find(reg, type);
        if (entry != nullptr && entry->readCB != nullptr) {
            if (entry->readCB(stream, static_cast<RwInt32>(chunkLength), object, entry->offset, entry->size) == nullptr) {
                return nullptr;
            }
        } else if (RwStreamSkip(stream, chunkLength) == nullptr) {
            return nullptr;
        }
        length -= chunkLength + kChunkHeaderSize;   // unsigned, as the original's
    }
    for (const RwPluginRegEntry* entry = reg->firstRegEntry; entry != nullptr; entry = entry->nextRegEntry) {
        if (entry->alwaysCB != nullptr && !entry->alwaysCB(object, entry->offset, entry->size)) return nullptr;
    }
    return reg;
}

const RwPluginRegistry* _rwPluginRegistryInvokeRights(const RwPluginRegistry* reg, RwUInt32 pluginID, void* object,
                                                      RwUInt32 extraData) {
    const RwPluginRegEntry* entry = find(reg, pluginID);
    if (entry == nullptr || entry->rightsCB == nullptr) return nullptr;
    return entry->rightsCB(object, entry->offset, entry->size, extraData) ? reg : nullptr;
}

RwInt32 _rwPluginRegistryGetSize(const RwPluginRegistry* reg, const void* object) {
    RwInt32 total = 0;
    for (const RwPluginRegEntry* entry = reg->firstRegEntry; entry != nullptr; entry = entry->nextRegEntry) {
        if (entry->getSizeCB == nullptr) continue;
        const RwInt32 size = entry->getSizeCB(object, entry->offset, entry->size);
        if (size > 0) total += size + static_cast<RwInt32>(kChunkHeaderSize);
    }
    return total;
}

const RwPluginRegistry* _rwPluginRegistryWriteDataChunks(const RwPluginRegistry* reg, RwStream* stream,
                                                         const void* object) {
    const RwInt32 total = _rwPluginRegistryGetSize(reg, object);
    if (_rwStreamWriteVersionedChunkHeader(stream, rwID_EXTENSION, total, kWriteVersion, kWriteBuild) == nullptr) {
        return nullptr;
    }
    for (const RwPluginRegEntry* entry = reg->firstRegEntry; entry != nullptr; entry = entry->nextRegEntry) {
        if (entry->getSizeCB == nullptr || entry->writeCB == nullptr) continue;
        const RwInt32 size = entry->getSizeCB(object, entry->offset, entry->size);
        if (size <= 0) continue;
        if (_rwStreamWriteVersionedChunkHeader(stream, static_cast<RwInt32>(entry->pluginID), size, kWriteVersion,
                                               kWriteBuild) == nullptr ||
            entry->writeCB(stream, size, object, entry->offset, entry->size) == nullptr) {
            return nullptr;
        }
    }
    return reg;
}

const RwPluginRegistry* _rwPluginRegistrySkipDataChunks(const RwPluginRegistry* reg, RwStream* stream) {
    RwUInt32 length = 0;
    if (!RwStreamFindChunk(stream, rwID_EXTENSION, &length, nullptr)) return nullptr;
    while (length != 0) {
        RwUInt32 chunkLength = 0;
        if (!_rwStreamReadChunkHeader(stream, nullptr, &chunkLength, nullptr, nullptr)) return nullptr;
        if (RwStreamSkip(stream, chunkLength) == nullptr) return nullptr;
        length -= chunkLength + kChunkHeaderSize;
    }
    return reg;
}
