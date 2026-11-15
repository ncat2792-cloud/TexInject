cmake_minimum_required(VERSION 3.22.1)
project("texinject" LANGUAGES C CXX ASM)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS} -ffunction-sections -fdata-sections -Os -fPIC")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -ffunction-sections -fdata-sections -Os -fPIC")
set(CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS} -fPIC")
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--gc-sections -Wl,--as-needed -Wl,-z,max-page-size=4096 -Wl,-z,noexecstack")
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

find_library(log-lib log)
find_library(dl-lib dl)

# ---------------------------------------------------------------------------
# 私有 CPython 2.7.13：只用来把 .py 编成游戏那个 opcode 表的字节码，
# 和游戏自己的解释器完全隔离，只有字节流跨过去。
# ---------------------------------------------------------------------------
add_subdirectory(python)

# ---------------------------------------------------------------------------
# Brotli：BDX / 无限压缩格式编解码，只有建筑导入导出用。
# ---------------------------------------------------------------------------
add_subdirectory(third_party_brotli)

# ---------------------------------------------------------------------------
# Dobby：建筑工具里有 19 处 DobbyInstrument（入口读寄存器的指令级插桩），
# 模块自己的 InlineHook 只能做头部跳转，表达不了，所以整套并存。
# ---------------------------------------------------------------------------
set(DOBBY_GENERATE_SHARED OFF CACHE BOOL "" FORCE)
set(DOBBY_DEBUG OFF CACHE BOOL "" FORCE)
set(Plugin.SymbolResolver OFF CACHE BOOL "" FORCE)
add_subdirectory(Dobby)

# OpenSSL 静态库：.infcz 建筑格式的签名/加密。
add_library(crypto STATIC IMPORTED)
set_target_properties(crypto PROPERTIES
        IMPORTED_LOCATION ${CMAKE_SOURCE_DIR}/libs/${ANDROID_ABI}/libcrypto.a)

file(GLOB OPENSSL_HEADERS ${CMAKE_SOURCE_DIR}/openssl_ndk/openssl/*.h)

# ---------------------------------------------------------------------------
# 主库 libtexinject.so：只保留 Python 执行桥
# ---------------------------------------------------------------------------
add_library(texinject SHARED
        native-lib.cpp
        tp/FunctionsAddress.cpp
        plt/PltHook.cpp
        trace/IoTrace.cpp
        mount/ResourceSourceHook.cpp
        mem/MemRw.cpp
        hook/InlineHook.cpp
        game/LocalPlayerHook.cpp
        game/GameApi.cpp
        common/Arm64SignatureScanner.cpp
        common/Signatures.cpp
        common/PyFunctionsAddress.cpp
        scripts/PyHost.cpp
        scripts/ScriptRuntime.cpp
        tp/TpModule.cpp

        # ---- 建筑工具：3.10.5 运行时地址解析 ----
        build_import/ContainerOpenPacketSenderThunks.S

        # ---- 建筑工具：游戏 hook ----
        tp/MinecraftUpdateHook.cpp
        tp/BuildPacketReceiveHook.cpp
        tp/LoopbackPacketSenderCapture.cpp

        Json/cJSON.c

        # ---- 建筑工具：导入 / 投影 / 打印 / 导出 ----
        build_import/BdxParser.cpp
        build_import/BdxWriter.cpp
        build_import/BlockCommandFormatter.cpp
        build_import/BlockMapper.cpp
        build_import/BuildExportCheckpoint.cpp
        build_import/BuildExportPaths.cpp
        build_import/BuildExportRuntime.cpp
        build_import/BuildImportCheckpoint.cpp
        build_import/BuildImportController.cpp
        build_import/BuildImportRuntime.cpp
        build_import/BuildImportUndoStore.cpp
        build_import/BuildProjectionRenderer.cpp
        build_import/BuildProjectionRuntime.cpp
        build_import/ChunkSpoolWriter.cpp
        build_import/CommandBlockPacketSender.cpp
        build_import/CommandBlockSpool.cpp
        build_import/CommandMusicParser.cpp
        build_import/CommandSpool.cpp
        build_import/ContainerCaptureMailbox.cpp
        build_import/ContainerClosePacketSender.cpp
        build_import/ContainerEntityCodec.cpp
        build_import/ContainerOpenPacketSender.cpp
        build_import/DeferredImportDataSpool.cpp
        build_import/DirectPlacementSender.cpp
        build_import/ExecuteCommandConverter.cpp
        build_import/InfiniteczBuildParser.cpp
        build_import/InfiniteczBuildWriter.cpp
        build_import/InfinityCompressionCodec.cpp
        build_import/InfinityCryptoCodec.cpp
        build_import/InfinityFormatKey.cpp
        build_import/ItemExtraMapMetadata.cpp
        build_import/ItemRuntimeRegistry.cpp
        build_import/LitematicParser.cpp
        build_import/MapAnvilClientSlotSync.cpp
        build_import/MapAnvilClientSyncDelivery.cpp
        build_import/MapAnvilCreatedOutputProbe.cpp
        build_import/MapAnvilDebugBridge.cpp
        build_import/MapAnvilNativeActionTrace.cpp
        build_import/MapAnvilRecipeProbe.cpp
        build_import/MapAnvilRecipeProbeState.cpp
        build_import/MapAnvilRenameJournal.cpp
        build_import/MapAnvilRenameSender.cpp
        build_import/MapAnvilUiCloseBridge.cpp
        build_import/MapAnvilWireDiagnostic.cpp
        build_import/MapBlankSupplyJournal.cpp
        build_import/MapChestClientSlotSync.cpp
        build_import/MapChestClientSyncDelivery.cpp
        build_import/MapChestNativeSurvey.cpp
        build_import/MapChestPlacement.cpp
        build_import/MapChestProductionPreflight.cpp
        build_import/MapChestStorageSender.cpp
        build_import/MapChestStorageVerification.cpp
        build_import/MapChestTransferJournal.cpp
        build_import/MapChestUiCloseBridge.cpp
        build_import/MapChestWindowSession.cpp
        build_import/MapChestWorldPlacement.cpp
        build_import/MapExtraChestPlacementJournal.cpp
        build_import/MapInventoryTransfer.cpp
        build_import/MapManualOutboundTrace.cpp
        build_import/MapNativeAnvilCraftPacket.cpp
        build_import/MapNativeAnvilManagerProbe.cpp
        build_import/MapNativeAnvilNameBridge.cpp
        build_import/MapNativeAnvilResultActionBridge.cpp
        build_import/MapNativeAnvilResultGroupProbe.cpp
        build_import/MapNativeHeldMapEvidence.cpp
        build_import/MapPairPlacementJournal.cpp
        build_import/MapStorageAnvilAdapter.cpp
        build_import/MapStorageChestCaptureAdapter.cpp
        build_import/MapStorageCoordinator.cpp
        build_import/MapStorageCursorCommit.cpp
        build_import/MapStoragePipelineDriver.cpp
        build_import/MapStorageProductionActions.cpp
        build_import/MapTextureObservation.cpp
        build_import/MapVisibleAnvilProductionPreflight.cpp
        build_import/MapVisibleAnvilWindowSession.cpp
        build_import/McworldParser.cpp
        build_import/MidiCommandMusicParser.cpp
        build_import/NativeWorldAccess.cpp
        build_import/PixelArtParser.cpp
        build_import/PlayerInventoryOpenPacketSender.cpp
        build_import/ProjectionBlockIdentity.cpp
        build_import/ProjectionBlockResolver.cpp
        build_import/ProjectionPrinterInventoryClientSync.cpp
        build_import/ProjectionPrinterInventoryMailbox.cpp
        build_import/ProjectionPrinterInventoryMover.cpp
        build_import/ProjectionPrinterInventoryPresentationGate.cpp
        build_import/ProjectionPrinterInventorySession.cpp
        build_import/ProjectionPrinterNativeHotbarSelection.cpp
        build_import/ProjectionPrinterRuntime.cpp
        build_import/ProjectionPrinterSilentRotation.cpp
        build_import/ProjectionWorldMatchRuntime.cpp
        build_import/PyRpcAckDecoder.cpp
        build_import/RenderCameraTracker.cpp
        build_import/SchematicParser.cpp
        build_import/SchematicWriter.cpp
        build_import/SignBlockActorPacketSender.cpp
        build_import/SignEditSessionMailbox.cpp
        build_import/SignEntityCodec.cpp
        ${OPENSSL_HEADERS})

target_include_directories(texinject PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/Dobby/include
        ${CMAKE_CURRENT_SOURCE_DIR}/Json
        ${CMAKE_CURRENT_SOURCE_DIR}/openssl_ndk
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party_brotli/c/include)
find_library(z-lib z)
find_library(glesv3-lib GLESv3)
find_library(egl-lib EGL)

target_link_libraries(texinject
        ${log-lib} ${dl-lib}
        pyloader_python27_compiler
        build_import_brotli_decoder
        build_import_brotli_encoder
        crypto
        dobby
        ${z-lib}
        ${glesv3-lib}
        ${egl-lib})

# 地图/铁砧那条传输链的调试追踪。
target_compile_definitions(texinject PRIVATE MAP_ANVIL_DEBUG_TRACE=1)
