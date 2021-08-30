# Oodle
Oodle src extracted from Unreal Engine. 

# Configuration

* **OodleCore (oo2core_9_)**: compiles a dll with oodle compression/decompression functions
* **OodleNetwork (oo2net_9_)**: compiles a dll with oodle compression/decompression functions + net related functions
* **OodleTexture (oo2tex_9_)**: compiles a dll with oodle compression/decompression functions + texture compression related functions
* **OodleNetworkAndTexture (oo2netex_9_)**: compiles a dll with oodle compression/decompression functions + net related functions + texture compression related functions

# Exported Functions

## oo2core_9_:

* OodleCore_Plugin_DisplayAssertion_Default
* OodleCore_Plugin_Free_Default
* OodleCore_Plugin_MallocAligned_Default
* OodleCore_Plugin_Printf_Default
* OodleCore_Plugin_Printf_Verbose
* OodleCore_Plugin_RunJob_Default
* OodleCore_Plugin_WaitJob_Default
* OodleCore_Plugins_SetAllocators
* OodleCore_Plugins_SetAssertion
* OodleCore_Plugins_SetJobSystem
* OodleCore_Plugins_SetJobSystemAndCount
* OodleCore_Plugins_SetPrintf
* OodleKraken_Decode_Headerless
* OodleLZDecoder_Create
* OodleLZDecoder_DecodeSome
* OodleLZDecoder_Destroy
* OodleLZDecoder_MakeValidCircularWindowSize
* OodleLZDecoder_MemorySizeNeeded
* OodleLZDecoder_Reset
* OodleLZLegacyVTable_InstallToCore
* OodleLZ_CheckSeekTableCRCs
* OodleLZ_Compress
* OodleLZ_CompressOptions_GetDefault
* OodleLZ_CompressOptions_Validate
* OodleLZ_CompressionLevel_GetName
* OodleLZ_Compressor_GetName
* OodleLZ_CreateSeekTable
* OodleLZ_Decompress
* OodleLZ_FillSeekTable
* OodleLZ_FindSeekEntry
* OodleLZ_FreeSeekTable
* OodleLZ_GetAllChunksCompressor
* OodleLZ_GetChunkCompressor
* OodleLZ_GetCompressScratchMemBound
* OodleLZ_GetCompressedBufferSizeNeeded
* OodleLZ_GetCompressedStepForRawStep
* OodleLZ_GetDecodeBufferSize
* OodleLZ_GetFirstChunkCompressor
* OodleLZ_GetInPlaceDecodeBufferSize
* OodleLZ_GetNumSeekChunks
* OodleLZ_GetSeekEntryPackedPos
* OodleLZ_GetSeekTableMemorySizeNeeded
* OodleLZ_Jobify_GetName
* OodleLZ_MakeSeekChunkLen
* OodleLZ_ThreadPhased_BlockDecoderMemorySizeNeeded
* Oodle_CheckVersion
* Oodle_GetConfigValues
* Oodle_LogHeader
* Oodle_SetConfigValues
* Oodle_SetUsageWarnings
 
 ## oo2net_9_ :  oo2core_9_ + 
 
* OodleNetwork1TCP_Decode
* OodleNetwork1TCP_Encode
* OodleNetwork1TCP_State_InitAsCopy
* OodleNetwork1TCP_State_Reset
* OodleNetwork1TCP_State_Size
* OodleNetwork1TCP_Train
* OodleNetwork1UDP_Decode
* OodleNetwork1UDP_Encode
* OodleNetwork1UDP_StateCompacted_MaxSize
* OodleNetwork1UDP_State_Compact
* OodleNetwork1UDP_State_Size
* OodleNetwork1UDP_State_Uncompact
* OodleNetwork1UDP_Train
* OodleNetwork1_CompressedBufferSizeNeeded
* OodleNetwork1_SelectDictionaryFromPackets
* OodleNetwork1_SelectDictionaryFromPackets_Trials
* OodleNetwork1_SelectDictionarySupported
* OodleNetwork1_Shared_SetWindow
* OodleNetwork1_Shared_Size

 ## oo2tex_9_ : oo2core_9_ + 
 *
 *
 *
 
 ## oo2netex_9_ : oo2core_9_ + oo2net_9_
