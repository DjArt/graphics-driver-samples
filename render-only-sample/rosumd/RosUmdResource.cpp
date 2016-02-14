///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//
// Resource implementation
//
// Copyright (C) Microsoft Corporation
//
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
#include "RosUmdDevice.h"
#include "RosUmdResource.h"
#include "RosUmdDebug.h"
#include "RosUmdLogging.h"

#include "RosContext.h"

#include "Vc4Hw.h"

RosUmdResource::RosUmdResource() :
    m_hKMAllocation(NULL)
{
    memset(&m_TileInfo, 0, sizeof(m_TileInfo));
}

RosUmdResource::~RosUmdResource()
{
    // do nothing
}

void
RosUmdResource::Standup(
    RosUmdDevice *pUmdDevice,
    const D3D11DDIARG_CREATERESOURCE* pCreateResource,
    D3D10DDI_HRTRESOURCE hRTResource)
{
    UNREFERENCED_PARAMETER(pUmdDevice);

    m_resourceDimension = pCreateResource->ResourceDimension;
    m_mip0Info = *pCreateResource->pMipInfoList;
    m_usage = pCreateResource->Usage;
    m_bindFlags = pCreateResource->BindFlags;
    m_mapFlags = pCreateResource->MapFlags;
    m_miscFlags = pCreateResource->MiscFlags;
    m_format = pCreateResource->Format;
    m_sampleDesc = pCreateResource->SampleDesc;
    m_mipLevels = pCreateResource->MipLevels;
    m_arraySize = pCreateResource->ArraySize;

    if (pCreateResource->pPrimaryDesc)
    {
        m_primaryDesc = *pCreateResource->pPrimaryDesc;
    }
    else
    {
        ZeroMemory(&m_primaryDesc, sizeof(m_primaryDesc));
    }

    CalculateMemoryLayout();

    m_hRTResource = hRTResource;

    // Zero out internal state
    m_hKMResource = 0;
    m_hKMAllocation = 0;

    // Mark that the resource is not referenced by a command buffer (.i.e. null fence value)
    m_mostRecentFence = RosUmdCommandBuffer::s_nullFence;

    m_allocationListIndex = 0;

    m_pSysMemCopy = NULL;
}

void
RosUmdResource::Teardown(void)
{
    // TODO[indyz]: Implement
}

void
RosUmdResource::ConstantBufferUpdateSubresourceUP(
    UINT DstSubresource,
    _In_opt_ const D3D10_DDI_BOX *pDstBox,
    _In_ const VOID *pSysMemUP,
    UINT RowPitch,
    UINT DepthPitch,
    UINT CopyFlags)
{
    assert(DstSubresource == 0);
    assert(pSysMemUP);

    assert(m_bindFlags & D3D10_DDI_BIND_CONSTANT_BUFFER); // must be constant buffer
    assert(m_resourceDimension == D3D10DDIRESOURCE_BUFFER);

    BYTE *pSysMemCopy = m_pSysMemCopy;
    UINT BytesToCopy = RowPitch;
    if (pDstBox)
    {
        if (pDstBox->left < 0 || 
            pDstBox->left > (INT)m_hwSizeBytes ||
            pDstBox->left > pDstBox->right ||
            pDstBox->right > (INT)m_hwSizeBytes)
        {
            return; // box is outside of buffer size. Nothing to copy.
        }

        pSysMemCopy += pDstBox->left;
        BytesToCopy = (pDstBox->right - pDstBox->left);
    }
    else if (BytesToCopy == 0)
    {
        BytesToCopy = m_hwSizeBytes; // copy whole.
    }
    else
    {
        BytesToCopy = min(BytesToCopy, m_hwSizeBytes);
    }

    CopyMemory(pSysMemCopy, pSysMemUP, BytesToCopy);

    return;

    DepthPitch;
    CopyFlags;
}

void
RosUmdResource::Map(
    RosUmdDevice *pUmdDevice,
    UINT subResource,
    D3D10_DDI_MAP mapType,
    UINT mapFlags,
    D3D10DDI_MAPPED_SUBRESOURCE* pMappedSubRes)
{
    assert(m_mipLevels <= 1);
    assert(m_arraySize == 1);

    UNREFERENCED_PARAMETER(subResource);

    //
    // Constant data is copied into command buffer, so there is no need for flushing
    //

    if (m_bindFlags & D3D10_DDI_BIND_CONSTANT_BUFFER)
    {
        pMappedSubRes->pData = m_pSysMemCopy;

        pMappedSubRes->RowPitch = m_hwPitchBytes;
        pMappedSubRes->DepthPitch = (UINT)m_hwSizeBytes;

        return;
    }

    pUmdDevice->m_commandBuffer.FlushIfMatching(m_mostRecentFence);

    D3DDDICB_LOCK lock;
    memset(&lock, 0, sizeof(lock));

    lock.hAllocation = m_hKMAllocation;

    //
    // TODO[indyz]: Consider how to optimize D3D10_DDI_MAP_WRITE_NOOVERWRITE
    //
    //    D3DDDICB_LOCKFLAGS::IgnoreSync and IgnoreReadSync are used for
    //    D3D10_DDI_MAP_WRITE_NOOVERWRITE optimization and are only allowed
    //    for allocations that can resides in aperture segment.
    //
    //    Currently ROS driver puts all allocations in local video memory.
    //

    SetLockFlags(mapType, mapFlags, &lock.Flags);

    pUmdDevice->Lock(&lock);

    if (lock.Flags.Discard)
    {
        assert(m_hKMAllocation != lock.hAllocation);

        m_hKMAllocation = lock.hAllocation;

        if (pUmdDevice->m_commandBuffer.IsResourceUsed(this))
        {
            //
            // Indicate that the new allocation instance of the resource
            // is not used in the current command batch.
            //

            m_mostRecentFence -= 1;
        }
    }

    pMappedSubRes->pData = lock.pData;
    m_pData = (BYTE*)lock.pData;

    pMappedSubRes->RowPitch = m_hwPitchBytes;
    pMappedSubRes->DepthPitch = (UINT)m_hwSizeBytes;
}

void
RosUmdResource::Unmap(
    RosUmdDevice *pUmdDevice,
    UINT subResource)
{
    UNREFERENCED_PARAMETER(subResource);

    if (m_bindFlags & D3D10_DDI_BIND_CONSTANT_BUFFER)
    {
        return;
    }

    m_pData = NULL;

    D3DDDICB_UNLOCK unlock;
    memset(&unlock, 0, sizeof(unlock));

    unlock.NumAllocations = 1;
    unlock.phAllocations = &m_hKMAllocation;

    pUmdDevice->Unlock(&unlock);
}

RosHwFormat 
MapDXGIFormatToHWFomat(DXGI_FORMAT format)
{
    // Map DXGI to internal format 
    // Currently only texture formats are handled

    if (format == DXGI_FORMAT_R8G8B8A8_UNORM)
    {
        return RosHwFormat::X8888;
    }

    if (format == DXGI_FORMAT_R8_UNORM)
    {
        return RosHwFormat::X8;
    }

    if (format == DXGI_FORMAT_R8G8_UNORM)
    {
        return RosHwFormat::X16;
    }
    
    //For other formats just return RosHWFormat::X8
    return RosHwFormat::X8;
}

void
RosUmdResource::FillTileInfo(UINT bpp)
{
    // Provide detailed information about tile.
    // Partial information about 4kB tiles, 1kB sub-tiles and micro-tiles for
    // given bpp is precalculated.
    // Values are used i.e. during converting bitmap to tiled texture

    if (bpp == 8)
    {
        m_TileInfo.VC4_1kBSubTileWidthPixels        = VC4_1KB_SUB_TILE_WIDTH_8BPP;
        m_TileInfo.VC4_1kBSubTileHeightPixels       = VC4_1KB_SUB_TILE_HEIGHT_8BPP;
        m_TileInfo.VC4_MicroTileWidthBytes          = VC4_MICRO_TILE_WIDTH_BYTES_8BPP;
        m_TileInfo.vC4_MicroTileHeight              = VC4_MICRO_TILE_HEIGHT_8BPP;
    }
    else
    if (bpp == 16)
    {
        m_TileInfo.VC4_1kBSubTileWidthPixels        = VC4_1KB_SUB_TILE_WIDTH_16BPP;
        m_TileInfo.VC4_1kBSubTileHeightPixels       = VC4_1KB_SUB_TILE_HEIGHT_16BPP;
        m_TileInfo.VC4_MicroTileWidthBytes          = VC4_MICRO_TILE_WIDTH_BYTES_16BPP;
        m_TileInfo.vC4_MicroTileHeight              = VC4_MICRO_TILE_HEIGHT_16BPP;
    }
    else
    if (bpp == 32)
    {
        m_TileInfo.VC4_1kBSubTileWidthPixels        = VC4_1KB_SUB_TILE_WIDTH_32BPP;
        m_TileInfo.VC4_1kBSubTileHeightPixels       = VC4_1KB_SUB_TILE_HEIGHT_32BPP;
        m_TileInfo.VC4_MicroTileWidthBytes          = VC4_MICRO_TILE_WIDTH_BYTES_32BPP;
        m_TileInfo.vC4_MicroTileHeight              = VC4_MICRO_TILE_HEIGHT_32BPP;
    }
    
    // Calculate sub-tile width in bytes
    m_TileInfo.VC4_1kBSubTileWidthBytes = m_TileInfo.VC4_1kBSubTileWidthPixels * (bpp / 8);
    
    // 4kB tile consists of four 1kB sub-tiles
    m_TileInfo.VC4_4kBTileWidthPixels = m_TileInfo.VC4_1kBSubTileWidthPixels * 2;
    m_TileInfo.VC4_4kBTileHeightPixels = m_TileInfo.VC4_1kBSubTileHeightPixels * 2;
    m_TileInfo.VC4_4kBTileWidthBytes = m_TileInfo.VC4_1kBSubTileWidthBytes * 2;
}

void
RosUmdResource::CalculateTilesInfo()
{
    UINT bpp = 0;

    m_hwFormat = MapDXGIFormatToHWFomat(m_format);

    // Calculate pixel bpp for given format
    switch (m_format)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
        {
            bpp = 32;
        }
        break;
    case DXGI_FORMAT_R8G8_UNORM:
        {
            bpp = 16;
        }
        break;
    case DXGI_FORMAT_R8_UNORM:
        {
            bpp = 8;
        }
        break;

    default:        
        bpp = 32;    
    }

    FillTileInfo(bpp);

    m_hwWidthTilePixels = m_TileInfo.VC4_4kBTileWidthPixels;
    m_hwHeightTilePixels = m_TileInfo.VC4_4kBTileHeightPixels;

    m_hwWidthTiles = (m_hwWidthPixels + m_hwWidthTilePixels - 1) / m_hwWidthTilePixels;
    m_hwHeightTiles = (m_hwHeightPixels + m_hwHeightTilePixels - 1) / m_hwHeightTilePixels;
    m_hwWidthPixels = m_hwWidthTiles*m_hwWidthTilePixels;
    m_hwHeightPixels = m_hwHeightTiles*m_hwHeightTilePixels;

    UINT sizeTileBytes = m_hwWidthTilePixels * m_hwHeightTilePixels * (bpp/8);

    m_hwSizeBytes = m_hwWidthTiles * m_hwHeightTiles * sizeTileBytes;
    m_hwPitchBytes = 0;

}

void
RosUmdResource::SetLockFlags(
    D3D10_DDI_MAP mapType,
    UINT mapFlags,
    D3DDDICB_LOCKFLAGS *pLockFlags)
{
    switch (mapType)
    {
    case D3D10_DDI_MAP_READ:
        pLockFlags->ReadOnly = 1;
        break;
    case D3D10_DDI_MAP_WRITE:
        pLockFlags->WriteOnly = 1;
        break;
    case D3D10_DDI_MAP_READWRITE:
        break;
    case D3D10_DDI_MAP_WRITE_DISCARD:
        pLockFlags->Discard = 1;
    case D3D10_DDI_MAP_WRITE_NOOVERWRITE:
        break;
    }

    if (mapFlags & D3D10_DDI_MAP_FLAG_DONOTWAIT)
    {
        pLockFlags->DonotWait = 1;
    }
}

void
RosUmdResource::CalculateMemoryLayout(
    void)
{
    switch (m_resourceDimension)
    {
    case D3D10DDIRESOURCE_BUFFER:
        {
            m_hwLayout = RosHwLayout::Linear;

            // TODO(bhouse) Need mapping code from resource DXGI format to hw format
            m_hwFormat = RosHwFormat::X8;

            m_hwWidthPixels = m_mip0Info.TexelWidth;
            m_hwHeightPixels = m_mip0Info.TexelHeight;

            assert(m_hwFormat == RosHwFormat::X8);
            assert(m_hwHeightPixels == 1);
            m_hwPitchBytes = m_hwSizeBytes = m_hwWidthPixels;
        }
    break;
    case D3D10DDIRESOURCE_TEXTURE2D:
        {
            if (m_usage == D3D10_DDI_USAGE_DEFAULT)
            {
                m_hwLayout = RosHwLayout::Tiled;
            }
            else
            {
                m_hwLayout = RosHwLayout::Linear;
            }

#if VC4

            // TODO[indyz]: Enable tiled render target
            if ((m_bindFlags & D3D10_DDI_BIND_RENDER_TARGET) ||
                (m_bindFlags & D3D10_DDI_BIND_SHADER_RESOURCE))
            {
                m_hwLayout = RosHwLayout::Linear;
            }

#endif

            // TODO(bhouse) Need mapping code from resource DXGI format to hw format
            if (m_bindFlags & D3D10_DDI_BIND_DEPTH_STENCIL)
            {
                m_hwFormat = RosHwFormat::D24S8;
            }
            else
            {
                m_hwFormat = RosHwFormat::X8888;
            }

            // Force tiled layout for given configuration only
            if ((m_usage == D3D10_DDI_USAGE_DEFAULT) &&
                (m_bindFlags == D3D10_DDI_BIND_SHADER_RESOURCE))
            {
                m_hwLayout = RosHwLayout::Tiled;
            }

            // Using system memory linear MipMap as example
            m_hwWidthPixels = m_mip0Info.TexelWidth;
            m_hwHeightPixels = m_mip0Info.TexelHeight;

#if VC4
            // Align width and height to VC4_BINNING_TILE_PIXELS for binning
#endif

            if (m_hwLayout == RosHwLayout::Linear)
            {
                m_hwWidthTilePixels = VC4_BINNING_TILE_PIXELS;
                m_hwHeightTilePixels = VC4_BINNING_TILE_PIXELS;
                m_hwWidthTiles = (m_hwWidthPixels + m_hwWidthTilePixels - 1) / m_hwWidthTilePixels;
                m_hwHeightTiles = (m_hwHeightPixels + m_hwHeightTilePixels - 1) / m_hwHeightTilePixels;
                m_hwWidthPixels = m_hwWidthTiles*m_hwWidthTilePixels;
                m_hwHeightPixels = m_hwHeightTiles*m_hwHeightTilePixels;

                m_hwSizeBytes = CPixel::ComputeMipMapSize(
                    m_hwWidthPixels,
                    m_hwHeightPixels,
                    m_mipLevels,
                    m_format);

                m_hwPitchBytes = CPixel::ComputeSurfaceStride(
                    m_hwWidthPixels,
                    CPixel::BytesPerPixel(m_format));
            }
            else
            {
                CalculateTilesInfo();
            }
        }
        break;
    case D3D10DDIRESOURCE_TEXTURE1D:
    case D3D10DDIRESOURCE_TEXTURE3D:
    case D3D10DDIRESOURCE_TEXTURECUBE:
        {
            throw RosUmdException(DXGI_DDI_ERR_UNSUPPORTED);
        }
        break;
    }
}

void RosUmdResource::GetAllocationExchange(
    RosAllocationExchange * pOutAllocationExchange)
{
#if 0
    pOutAllocationExchange->m_resourceDimension = m_resourceDimension;
#endif
    pOutAllocationExchange->m_mip0Info = m_mip0Info;
#if 0
    pOutAllocationExchange->m_usage = m_usage;
    pOutAllocationExchange->m_mapFlags = m_mapFlags;
    pOutAllocationExchange->m_miscFlags = m_miscFlags;
#endif
    pOutAllocationExchange->m_bindFlags = m_bindFlags;
    pOutAllocationExchange->m_format = m_format;
    pOutAllocationExchange->m_sampleDesc = m_sampleDesc;
#if 0
    pOutAllocationExchange->m_mipLevels = m_mipLevels;
    pOutAllocationExchange->m_arraySize = m_arraySize;
#endif
    pOutAllocationExchange->m_primaryDesc = m_primaryDesc;
    pOutAllocationExchange->m_hwLayout = m_hwLayout;
    pOutAllocationExchange->m_hwWidthPixels = m_hwWidthPixels;
    pOutAllocationExchange->m_hwHeightPixels = m_hwHeightPixels;
    pOutAllocationExchange->m_hwFormat = m_hwFormat;
#if 0
    pOutAllocationExchange->m_hwPitch = m_hwPitch;
#endif

    pOutAllocationExchange->m_hwSizeBytes = m_hwSizeBytes;
}

// Form 1k sub-tile block
BYTE *RosUmdResource::Form1kSubTileBlock(BYTE *pInputBuffer, BYTE *pOutBuffer, UINT rowStride)
{    
    // 1k sub-tile block is formed from micro-tiles blocks
    for (UINT h = 0; h < m_TileInfo.VC4_1kBSubTileHeightPixels; h += m_TileInfo.vC4_MicroTileHeight)
    {
        BYTE *currentBufferPos = pInputBuffer + h*rowStride;

        // Process row of 4 micro-tiles blocks
        for (UINT w = 0; w < m_TileInfo.VC4_1kBSubTileWidthBytes; w+= m_TileInfo.VC4_MicroTileWidthBytes)
        {
            BYTE *microTileOffset = currentBufferPos + w;

            // Process micro-tile block
            for (UINT t = 0; t < m_TileInfo.vC4_MicroTileHeight; t++)
            {
                memcpy(pOutBuffer, microTileOffset, m_TileInfo.VC4_MicroTileWidthBytes);
                pOutBuffer += m_TileInfo.VC4_MicroTileWidthBytes;
                microTileOffset += rowStride;
            }
        }
    }
    return pOutBuffer;
}

// Form one 4k tile block from pInputBuffer and store in pOutBuffer
BYTE *RosUmdResource::Form4kTileBlock(BYTE *pInputBuffer, BYTE *pOutBuffer, UINT rowStride, BOOLEAN OddRow)
{
    BYTE *currentTileOffset = NULL;
   
    UINT subTileHeightPixels        = m_TileInfo.VC4_1kBSubTileHeightPixels;
    UINT subTileWidthBytes          = m_TileInfo.VC4_1kBSubTileWidthBytes;

    if (OddRow)
    {
        // For even rows, process sub-tile blocks in ABCD order, where
        // each sub-tile is stored in memory as follows:
        //
        //  [C  B]   
        //  [D  A]
        //                  

        // Get A block
        currentTileOffset = pInputBuffer + rowStride * subTileHeightPixels + subTileWidthBytes;
        pOutBuffer = Form1kSubTileBlock(currentTileOffset, pOutBuffer, rowStride);

        // Get B block
        currentTileOffset = pInputBuffer + subTileWidthBytes;

        pOutBuffer = Form1kSubTileBlock(currentTileOffset, pOutBuffer, rowStride);

        // Get C block
        pOutBuffer = Form1kSubTileBlock(pInputBuffer, pOutBuffer, rowStride);

        // Get D block
        currentTileOffset = pInputBuffer + rowStride * subTileHeightPixels;
        pOutBuffer = Form1kSubTileBlock(currentTileOffset, pOutBuffer, rowStride);

        // return current position in out buffer
        return pOutBuffer;

    }
    else
    {
        // For even rows, process sub-tile blocks in ABCD order, where
        // each sub-tile is stored in memory as follows:
        // 
        //  [A  D]    
        //  [B  C] 
        //

        // Get A block
        pOutBuffer = Form1kSubTileBlock(pInputBuffer, pOutBuffer, rowStride);

        /// Get B block
        currentTileOffset = pInputBuffer + rowStride * subTileHeightPixels;
        pOutBuffer = Form1kSubTileBlock(currentTileOffset, pOutBuffer, rowStride);

        // Get C Block
        currentTileOffset = pInputBuffer + rowStride * subTileHeightPixels + subTileWidthBytes;
        pOutBuffer = Form1kSubTileBlock(currentTileOffset, pOutBuffer, rowStride);

        // Get D block
        currentTileOffset = pInputBuffer + subTileWidthBytes;
        pOutBuffer = Form1kSubTileBlock(currentTileOffset, pOutBuffer, rowStride);

        // return current position in out buffer
        return pOutBuffer;
    }
}

// Form (CountX * CountY) tile blocks from InputBuffer and store them in OutBuffer
void RosUmdResource::ConvertBitmapTo4kTileBlocks(BYTE *InputBuffer, BYTE *OutBuffer, UINT rowStride)
{
    UINT CountX = m_hwWidthTiles;
    UINT CountY = m_hwHeightTiles;

    for (UINT k = 0; k < CountY; k++)
    {
        BOOLEAN oddRow = k & 1;
        if (oddRow)
        {
            // Build 4k blocks from right to left for odd rows
            for (int i = CountX - 1; i >= 0; i--)
            {
                BYTE *blockStartOffset = InputBuffer + k * rowStride * m_TileInfo.VC4_4kBTileHeightPixels + i * m_TileInfo.VC4_4kBTileWidthBytes;
                OutBuffer = Form4kTileBlock(blockStartOffset, OutBuffer, rowStride, oddRow);
            }
        }
        else
        {
            // Build 4k blocks from left to right for even rows
            for (UINT i = 0; i < CountX; i++)
            {
                BYTE *blockStartOffset = InputBuffer + k * rowStride * m_TileInfo.VC4_4kBTileHeightPixels + i * m_TileInfo.VC4_4kBTileWidthBytes;
                OutBuffer = Form4kTileBlock(blockStartOffset, OutBuffer, rowStride, oddRow);
            }
        }
    }
}
