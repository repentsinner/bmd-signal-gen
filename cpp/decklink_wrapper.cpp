#include "decklink_wrapper.h"
#include "pixel_packing.h"
#include <iostream>
#include <algorithm>
#include <cstring>
#include "DeckLinkAPIVersion.h"
#include <CoreFoundation/CoreFoundation.h>

// Helper function to convert a 32-bit integer to 4-character ASCII code
std::string fourCharCode(int value) {
    char chars[7] = {
        '\'',
        static_cast<char>((value >> 24) & 0xFF),
        static_cast<char>((value >> 16) & 0xFF),
        static_cast<char>((value >> 8) & 0xFF),
        static_cast<char>(value & 0xFF),
        '\'',
        '\0'
    };
    return std::string(chars);
}

// DeckLinkSignalGen Implementation
DeckLinkSignalGen::DeckLinkSignalGen() 
    : m_device(nullptr)
    , m_output(nullptr)
    , m_frame(nullptr)
    , m_width(1920)
    , m_height(1080)
    , m_outputEnabled(false)
    , m_pixelFormat(bmdFormat12BitRGBLE)
    , m_formatsCached(false)
{
    // Initialize HDR metadata with default Rec2020 values (matching SignalGenHDR sample)
    m_hdrMetadata.EOTF = 2; // PQ
    m_hdrMetadata.referencePrimaries.RedX = 0.708;
    m_hdrMetadata.referencePrimaries.RedY = 0.292;
    m_hdrMetadata.referencePrimaries.GreenX = 0.170;
    m_hdrMetadata.referencePrimaries.GreenY = 0.797;
    m_hdrMetadata.referencePrimaries.BlueX = 0.131;
    m_hdrMetadata.referencePrimaries.BlueY = 0.046;
    m_hdrMetadata.referencePrimaries.WhiteX = 0.3127;
    m_hdrMetadata.referencePrimaries.WhiteY = 0.3290;
    m_hdrMetadata.maxDisplayMasteringLuminance = 1000.0;
    m_hdrMetadata.minDisplayMasteringLuminance = 0.0001;
    m_hdrMetadata.maxCLL = 1000.0;
    m_hdrMetadata.maxFALL = 50.0;
}

DeckLinkSignalGen::~DeckLinkSignalGen() {
    if (m_outputEnabled) {
        stopOutput();
    }
    if (m_frame) {
        m_frame->Release();
        m_frame = nullptr;
    }
    if (m_output) {
        m_output->Release();
        m_output = nullptr;
    }
    if (m_device) {
        m_device->Release();
        m_device = nullptr;
    }
    m_formatsCached = false;
    m_supportedFormats.clear();
}

void DeckLinkSignalGen::logFrameInfo(const char* context) {
    if (m_frame) {
        BMDFrameFlags flags = m_frame->GetFlags();
        uint16_t width = m_frame->GetWidth();
        uint16_t height = m_frame->GetHeight();
        uint16_t rowBytes = m_frame->GetRowBytes();
        BMDPixelFormat format = m_frame->GetPixelFormat();
        
        std::cerr << "[DeckLink] Frame info " << context << ":" << std::endl;
        std::cerr << "  Width: " << std::dec << width << ", Height: " << std::dec << height << std::endl;
        std::cerr << "  RowBytes: " << std::dec << rowBytes << std::endl;
        std::cerr << "  PixelFormat: " << fourCharCode(static_cast<int>(format)) << std::endl;
        std::cerr << "  Flags: 0x" << std::hex << flags << std::dec << std::endl;
    } else {
        std::cerr << "[DeckLink] No frame available for logging" << std::endl;
    }
}

/**
 * @brief Enables video output on the DeckLink device
 * 
 * This method enables video output on the DeckLink device in HD1080p30 mode
 * with default video output flags. This is the first step in starting output.
 * 
 * @return int Returns 0 on success, negative values on failure:
 *         - -1: Output interface not available or already enabled
 * 
 * @see stopOutput(), createFrame(), scheduleFrame(), startPlayback()
 */
int DeckLinkSignalGen::startOutput() {
    if (!m_output) return -1;
    if (m_outputEnabled) return 0;
    
    // Enable video output
    HRESULT enableResult = m_output->EnableVideoOutput(bmdModeHD1080p30, bmdVideoOutputFlagDefault);
    if (enableResult != S_OK) {
        std::cerr << "[DeckLink] EnableVideoOutput failed. HRESULT: 0x" << std::hex << enableResult << std::dec << std::endl;
        return -1;
    }
    m_outputEnabled = true;
    
    return 0;
}

int DeckLinkSignalGen::stopOutput() {
    if (!m_outputEnabled) return 0;
    
    m_output->StopScheduledPlayback(0, nullptr, 0);
    m_output->DisableVideoOutput();
    m_outputEnabled = false;
    
    return 0;
}

int DeckLinkSignalGen::createFrame() {
    if (!m_output || !m_outputEnabled) return -1;
    if (m_pendingFrameData.empty()) {
        std::cerr << "[DeckLink] No pending frame data available" << std::endl;
        return -2;
    }
    
    int32_t rowBytes = 0;
    // Debug output for pixel format and dimensions
    std::cerr << "[DeckLink][DEBUG] Calling RowBytesForPixelFormat with:"
              << " pixelFormat=" << fourCharCode(static_cast<int>(m_pixelFormat))
              << ", width=" << m_width
              << ", height=" << m_height << std::endl;
    HRESULT result = m_output->RowBytesForPixelFormat(m_pixelFormat, m_width, &rowBytes);
    if (result != S_OK) {
        std::cerr << "[DeckLink] RowBytesForPixelFormat failed. HRESULT: 0x"
                  << std::hex << result << std::dec << std::endl;
        std::cerr << "[DeckLink][DEBUG] Arguments were: pixelFormat="
                  << fourCharCode(static_cast<int>(m_pixelFormat))
                  << ", width=" << m_width
                  << ", height=" << m_height << std::endl;
        return -3;
    }
    // Output rowBytes in decimal notation
    std::cerr << "[DeckLink][DEBUG] RowBytesForPixelFormat returned rowBytes="
              << rowBytes << std::endl;
    
    result = m_output->CreateVideoFrame(
        m_width, m_height, rowBytes,
        m_pixelFormat,
        bmdFrameFlagDefault,
        &m_frame);
    if (!m_frame) {
        std::cerr << "[DeckLink] CreateVideoFrame failed" << std::endl;
        return -4;
    }
    
    // Get frame buffer for writing
    IDeckLinkVideoBuffer* videoBuffer = nullptr;
    if (m_frame->QueryInterface(IID_IDeckLinkVideoBuffer, (void**)&videoBuffer) != S_OK) {
        std::cerr << "[DeckLink] QueryInterface for IDeckLinkVideoBuffer failed" << std::endl;
        return -5;
    }
    
    if (videoBuffer->StartAccess(bmdBufferAccessWrite) != S_OK) {
        std::cerr << "[DeckLink] StartAccess failed" << std::endl;
        videoBuffer->Release();
        return -6;
    }
    
    void* frameData = nullptr;
    if (videoBuffer->GetBytes(&frameData) != S_OK) {
        std::cerr << "[DeckLink] GetBytes failed" << std::endl;
        videoBuffer->EndAccess(bmdBufferAccessWrite);
        videoBuffer->Release();
        return -7;
    }
    
    // Use pixel packing system to convert raw RGB data to the target format
    const uint16_t* srcData = m_pendingFrameData.data();
    
    // Pack the data according to the pixel format
    int err = pack_pixel_format(
                frameData,
                m_pixelFormat,
                srcData,
                m_width, m_height,
                rowBytes);
    
    videoBuffer->EndAccess(bmdBufferAccessWrite);
    videoBuffer->Release();
    if (err)
        return err;

    // Apply EOTF metadata if set
    if (m_hdrMetadata.EOTF >= 0) { // Changed from m_eotfType to m_hdrMetadata.EOTF
        applyHDRMetadata(); // Changed from applyEOTFMetadata to applyHDRMetadata
    }
    
    logFrameInfo("created");
    return 0;
}

int DeckLinkSignalGen::scheduleFrame() {
    if (!m_output || !m_frame) return -1;
    
    // Schedule the frame for output
    HRESULT result = m_output->ScheduleVideoFrame(
        m_frame,
        0,    // displayTime - start time
        1000, // displayDuration in units (1/30s at 30fps)
        30000 // timeScale (30000 units per second)
    );
    if (result != S_OK) {
        std::cerr << "[DeckLink] ScheduleVideoFrame failed. HRESULT: 0x" << std::hex << result << std::dec << std::endl;
        return -1;
    }
    
    logFrameInfo("scheduled");
    return 0;
}

int DeckLinkSignalGen::startPlayback() {
    if (!m_output) return -1;
    
    // Start scheduled playback
    HRESULT result = m_output->StartScheduledPlayback(0, 30000, 1.0);
    if (result != S_OK) {
        std::cerr << "[DeckLink] StartScheduledPlayback failed. HRESULT: 0x" << std::hex << result << std::dec << std::endl;
        return -1;
    }
    
    std::cerr << "[DeckLink] Playback started successfully" << std::endl;
    return 0;
}

int DeckLinkSignalGen::setPixelFormat(int pixelFormatIndex) {
    if (!m_output) return -1;
    
    if (!m_formatsCached) {
        cacheSupportedFormats();
    }
    
    if (pixelFormatIndex < 0 || pixelFormatIndex >= static_cast<int>(m_supportedFormats.size())) {
        std::cerr << "[DeckLink] Invalid pixel format index: " << pixelFormatIndex << std::endl;
        return -1;
    }
    
    m_pixelFormat = m_supportedFormats[pixelFormatIndex];
    std::cerr << "[DeckLink] Set pixel format to API index " << pixelFormatIndex
              << ", " << fourCharCode(static_cast<int>(m_pixelFormat)) << ")" << std::endl;
    
    return 0;
}

int DeckLinkSignalGen::getPixelFormat() const {
    if (!m_formatsCached) return -1;
    
    // Find the index of the current pixel format
    for (int i = 0; i < static_cast<int>(m_supportedFormats.size()); i++) {
        if (m_supportedFormats[i] == m_pixelFormat) {
            return i;
        }
    }
    
    return -1;
}

int DeckLinkSignalGen::setHDRMetadata(const HDRMetadata& metadata) {
    m_hdrMetadata = metadata;
    
    std::cerr << "[DeckLink] Set complete HDR metadata:" << std::endl;
    std::cerr << "  EOTF: " << metadata.EOTF << std::endl;
    std::cerr << "  Red Primary: (" << metadata.referencePrimaries.RedX << ", " << metadata.referencePrimaries.RedY << ")" << std::endl;
    std::cerr << "  Green Primary: (" << metadata.referencePrimaries.GreenX << ", " << metadata.referencePrimaries.GreenY << ")" << std::endl;
    std::cerr << "  Blue Primary: (" << metadata.referencePrimaries.BlueX << ", " << metadata.referencePrimaries.BlueY << ")" << std::endl;
    std::cerr << "  White Point: (" << metadata.referencePrimaries.WhiteX << ", " << metadata.referencePrimaries.WhiteY << ")" << std::endl;
    std::cerr << "  Max Display Mastering Luminance: " << metadata.maxDisplayMasteringLuminance << std::endl;
    std::cerr << "  Min Display Mastering Luminance: " << metadata.minDisplayMasteringLuminance << std::endl;
    std::cerr << "  MaxCLL: " << metadata.maxCLL << std::endl;
    std::cerr << "  MaxFALL: " << metadata.maxFALL << std::endl;
    
    return 0;
}

int DeckLinkSignalGen::setFrameData(const uint16_t* data, int width, int height) {
    if (!data || width <= 0 || height <= 0) return -1;
    // Update dimensions if they changed
    if (width != m_width || height != m_height) {
        m_width = width;
        m_height = height;
        std::cerr << "[DeckLink] Updated frame dimensions to " << width << "x" << height << std::endl;
    }
    // Store the frame data
    size_t dataSize = width * height * 3; // 3 channels (R, G, B) per pixel
    m_pendingFrameData.assign(data, data + dataSize);
    std::cerr << "[DeckLink] Set frame data: " << width << "x" << height
              << " pixels, " << m_pendingFrameData.size() << " samples" << std::endl;
    return 0;
}

int DeckLinkSignalGen::getDeviceCount() {
    IDeckLinkIterator* iterator = CreateDeckLinkIteratorInstance();
    if (!iterator) return 0;
    
    int count = 0;
    IDeckLink* device = nullptr;
    while (iterator->Next(&device) == S_OK) {
        count++;
        device->Release();
    }
    iterator->Release();
    
    return count;
}

std::string DeckLinkSignalGen::getDeviceName(int deviceIndex) {
    IDeckLinkIterator* iterator = CreateDeckLinkIteratorInstance();
    if (!iterator) return "";
    
    IDeckLink* device = nullptr;
    int current = 0;
    std::string deviceName;
    
    while (iterator->Next(&device) == S_OK) {
        if (current == deviceIndex) {
            CFStringRef nameRef = nullptr;
            if (device->GetDisplayName(&nameRef) == S_OK && nameRef) {
                char nameBuffer[256];
                CFStringGetCString(nameRef, nameBuffer, sizeof(nameBuffer), kCFStringEncodingUTF8);
                deviceName = nameBuffer;
                CFRelease(nameRef);
            }
            device->Release();
            break;
        }
        device->Release();
        current++;
    }
    iterator->Release();
    
    return deviceName;
}

void DeckLinkSignalGen::cacheSupportedFormats() {
    if (!m_output || m_formatsCached) return;
    
    std::set<BMDPixelFormat> uniqueFormats;
    
    // Test common pixel formats
    BMDPixelFormat supportedFormats[] = {
        bmdFormat8BitYUV,       // '2vuy' 4:2:2 Representation
        bmdFormat10BitYUV,      // 'v210' 4:2:2 Representation
        bmdFormat10BitYUVA,     // 'Ay10' 4:2:2 raw
        
        bmdFormat8BitARGB,      // 32     4:4:4:4 raw
        bmdFormat8BitBGRA,      // 'BGRA' 4:4:4:x raw

        bmdFormat10BitRGB,      // 'r210' 4:4:4 raw
        bmdFormat12BitRGB,      // 'R12B' Big-endian RGB 12-bit per component with full range (0-4095). Packed as 12-bit per component
        bmdFormat12BitRGBLE,    // 'R12L' Little-endian RGB 12-bit per component with full range (0-4095). Packed as 12-bit per component.
        
        bmdFormat10BitRGBXLE,   // 'R10l' 4:4:4 raw Three 10-bit unsigned components are packed into one 32-bit little-endian word.
        bmdFormat10BitRGBX,     // 'R10b' 4:4:4 raw Three 10-bit unsigned components are packed into one 32-bit big-endian word.
    };
    
    for (int i = 0; i < sizeof(supportedFormats)/sizeof(supportedFormats[0]); i++) {
        BMDPixelFormat format = supportedFormats[i];
        BMDDisplayMode actualMode;
        bool supported;
        
        if (m_output->DoesSupportVideoMode(bmdVideoConnectionUnspecified, 
                                          bmdModeHD1080p30, 
                                          format, 
                                          bmdNoVideoOutputConversion, 
                                          bmdSupportedVideoModeDefault, 
                                          &actualMode, 
                                          &supported) == S_OK && supported) {
            uniqueFormats.insert(format);
        }
    }
    
    m_supportedFormats.assign(uniqueFormats.begin(), uniqueFormats.end());
    m_formatsCached = true;
}

int DeckLinkSignalGen::applyHDRMetadata() {
    if (!m_frame) return 0;
    
    // Get the metadata extensions interface
    IDeckLinkVideoFrameMutableMetadataExtensions* metadataExt = nullptr;
    HRESULT result = m_frame->QueryInterface(IID_IDeckLinkVideoFrameMutableMetadataExtensions, (void**)&metadataExt);
    if (result != S_OK || !metadataExt) {
        std::cerr << "[DeckLink] Warning: Could not get metadata extensions interface (HRESULT: 0x" 
                  << std::hex << result << std::dec << "). HDR metadata will not be applied." << std::endl;
        return 0; // Don't fail the frame creation, just skip metadata
    }
    
    // Set colorspace metadata (Rec2020 for HDR)
    result = metadataExt->SetInt(bmdDeckLinkFrameMetadataColorspace, bmdColorspaceRec2020);
    if (result != S_OK) {
        std::cerr << "[DeckLink] Warning: Failed to set colorspace metadata (HRESULT: 0x" 
                  << std::hex << result << std::dec << ")" << std::endl;
    }
    
    // Set EOTF metadata
    result = metadataExt->SetInt(bmdDeckLinkFrameMetadataHDRElectroOpticalTransferFunc, m_hdrMetadata.EOTF);
    if (result != S_OK) {
        std::cerr << "[DeckLink] Warning: Failed to set EOTF metadata (HRESULT: 0x" 
                  << std::hex << result << std::dec << ")" << std::endl;
    }
    
    // Only apply full HDR metadata for PQ (EOTF = 2)
    if (m_hdrMetadata.EOTF == 2) {
        // Set the HDR metadata flag
        BMDFrameFlags currentFlags = m_frame->GetFlags();
        m_frame->SetFlags(currentFlags | bmdFrameContainsHDRMetadata);
        
        // Set display primaries
        result = metadataExt->SetFloat(bmdDeckLinkFrameMetadataHDRDisplayPrimariesRedX, m_hdrMetadata.referencePrimaries.RedX);
        if (result != S_OK) std::cerr << "[DeckLink] Warning: Failed to set RedX" << std::endl;
        
        result = metadataExt->SetFloat(bmdDeckLinkFrameMetadataHDRDisplayPrimariesRedY, m_hdrMetadata.referencePrimaries.RedY);
        if (result != S_OK) std::cerr << "[DeckLink] Warning: Failed to set RedY" << std::endl;
        
        result = metadataExt->SetFloat(bmdDeckLinkFrameMetadataHDRDisplayPrimariesGreenX, m_hdrMetadata.referencePrimaries.GreenX);
        if (result != S_OK) std::cerr << "[DeckLink] Warning: Failed to set GreenX" << std::endl;
        
        result = metadataExt->SetFloat(bmdDeckLinkFrameMetadataHDRDisplayPrimariesGreenY, m_hdrMetadata.referencePrimaries.GreenY);
        if (result != S_OK) std::cerr << "[DeckLink] Warning: Failed to set GreenY" << std::endl;
        
        result = metadataExt->SetFloat(bmdDeckLinkFrameMetadataHDRDisplayPrimariesBlueX, m_hdrMetadata.referencePrimaries.BlueX);
        if (result != S_OK) std::cerr << "[DeckLink] Warning: Failed to set BlueX" << std::endl;
        
        result = metadataExt->SetFloat(bmdDeckLinkFrameMetadataHDRDisplayPrimariesBlueY, m_hdrMetadata.referencePrimaries.BlueY);
        if (result != S_OK) std::cerr << "[DeckLink] Warning: Failed to set BlueY" << std::endl;
        
        // Set white point
        result = metadataExt->SetFloat(bmdDeckLinkFrameMetadataHDRWhitePointX, m_hdrMetadata.referencePrimaries.WhiteX);
        if (result != S_OK) std::cerr << "[DeckLink] Warning: Failed to set WhiteX" << std::endl;
        
        result = metadataExt->SetFloat(bmdDeckLinkFrameMetadataHDRWhitePointY, m_hdrMetadata.referencePrimaries.WhiteY);
        if (result != S_OK) std::cerr << "[DeckLink] Warning: Failed to set WhiteY" << std::endl;
        
        // Set mastering display luminance
        result = metadataExt->SetFloat(bmdDeckLinkFrameMetadataHDRMaxDisplayMasteringLuminance, m_hdrMetadata.maxDisplayMasteringLuminance);
        if (result != S_OK) std::cerr << "[DeckLink] Warning: Failed to set MaxDisplayMasteringLuminance" << std::endl;
        
        result = metadataExt->SetFloat(bmdDeckLinkFrameMetadataHDRMinDisplayMasteringLuminance, m_hdrMetadata.minDisplayMasteringLuminance);
        if (result != S_OK) std::cerr << "[DeckLink] Warning: Failed to set MinDisplayMasteringLuminance" << std::endl;
        
        // Set content light level
        result = metadataExt->SetFloat(bmdDeckLinkFrameMetadataHDRMaximumContentLightLevel, m_hdrMetadata.maxCLL);
        if (result != S_OK) std::cerr << "[DeckLink] Warning: Failed to set MaxCLL" << std::endl;
        
        result = metadataExt->SetFloat(bmdDeckLinkFrameMetadataHDRMaximumFrameAverageLightLevel, m_hdrMetadata.maxFALL);
        if (result != S_OK) std::cerr << "[DeckLink] Warning: Failed to set MaxFALL" << std::endl;
    } else {
        // Remove HDR metadata flag for non-PQ EOTF
        BMDFrameFlags currentFlags = m_frame->GetFlags();
        m_frame->SetFlags(currentFlags & ~bmdFrameContainsHDRMetadata);
    }
    
    metadataExt->Release();
    std::cerr << "[DeckLink] HDR metadata applied successfully" << std::endl;
    return 0;
}

// Thin C wrapper implementation
extern "C" {

int decklink_set_frame_data(DeckLinkHandle handle, const uint16_t* data, int width, int height) {
    if (!handle || !data) return -1;
    auto* signalGen = static_cast<DeckLinkSignalGen*>(handle);
    return signalGen->setFrameData(data, width, height);
}

int decklink_get_device_count() {
    return DeckLinkSignalGen::getDeviceCount();
}

int decklink_get_device_name_by_index(int index, char* name, int name_size) {
    if (!name || name_size <= 0) return -1;
    
    std::string deviceName = DeckLinkSignalGen::getDeviceName(index);
    if (deviceName.empty()) return -1;
    
    strncpy(name, deviceName.c_str(), name_size - 1);
    name[name_size - 1] = '\0';
    return 0;
}

DeckLinkHandle decklink_open_output_by_index(int index) {
    DeckLinkSignalGen* signalGen = new DeckLinkSignalGen();
    
    // Open the device
    IDeckLinkIterator* iterator = CreateDeckLinkIteratorInstance();
    if (!iterator) {
        delete signalGen;
        return nullptr;
    }
    
    IDeckLink* device = nullptr;
    int current = 0;
    while (iterator->Next(&device) == S_OK) {
        if (current == index) {
            IDeckLinkOutput* output = nullptr;
            if (device->QueryInterface(IID_IDeckLinkOutput, (void**)&output) == S_OK) {
                signalGen->m_device = device;
                signalGen->m_output = output;
                iterator->Release();
                return signalGen;
            }
            device->Release();
            break;
        }
        device->Release();
        current++;
    }
    iterator->Release();
    delete signalGen;
    return nullptr;
}

void decklink_close(DeckLinkHandle handle) {
    if (handle) {
        auto* signalGen = static_cast<DeckLinkSignalGen*>(handle);
        delete signalGen;
    }
}

int decklink_start_output(DeckLinkHandle handle) {
    if (!handle) return -1;
    auto* signalGen = static_cast<DeckLinkSignalGen*>(handle);
    return signalGen->startOutput();
}

int decklink_stop_output(DeckLinkHandle handle) {
    if (!handle) return -1;
    auto* signalGen = static_cast<DeckLinkSignalGen*>(handle);
    return signalGen->stopOutput();
}

int decklink_create_frame_from_data(DeckLinkHandle handle) {
    if (!handle) return -1;
    auto* signalGen = static_cast<DeckLinkSignalGen*>(handle);
    return signalGen->createFrame();
}

int decklink_schedule_frame_for_output(DeckLinkHandle handle) {
    if (!handle) return -1;
    auto* signalGen = static_cast<DeckLinkSignalGen*>(handle);
    return signalGen->scheduleFrame();
}

int decklink_start_scheduled_playback(DeckLinkHandle handle) {
    if (!handle) return -1;
    auto* signalGen = static_cast<DeckLinkSignalGen*>(handle);
    return signalGen->startPlayback();
}

int decklink_get_pixel_format(DeckLinkHandle handle) {
    if (!handle) return -1;
    auto* signalGen = static_cast<DeckLinkSignalGen*>(handle);
    return signalGen->getPixelFormat();
}

int decklink_set_pixel_format(DeckLinkHandle handle, int pixel_format_index) {
    if (!handle) return -1;
    auto* signalGen = static_cast<DeckLinkSignalGen*>(handle);
    return signalGen->setPixelFormat(pixel_format_index);
}

int decklink_get_supported_pixel_format_count(DeckLinkHandle handle) {
    if (!handle) return 0;
    auto* signalGen = static_cast<DeckLinkSignalGen*>(handle);
    signalGen->cacheSupportedFormats();
    return static_cast<int>(signalGen->getSupportedFormats().size());
}

int decklink_get_supported_pixel_format_name(DeckLinkHandle handle, int index, char* name, int name_size) {
    if (!handle || !name || name_size <= 0) return -1;
    auto* signalGen = static_cast<DeckLinkSignalGen*>(handle);
    
    signalGen->cacheSupportedFormats();
    if (index < 0 || index >= static_cast<int>(signalGen->getSupportedFormats().size())) return -1;
    
    BMDPixelFormat format = signalGen->getSupportedFormats()[index];
    std::string fmtChars = fourCharCode(static_cast<int>(format));
    std::string formatName;
    
    switch (format) {
        case bmdFormat8BitYUV: formatName = std::string("8Bit YUV (") + fmtChars + ")"; break;
        case bmdFormat10BitYUV: formatName = std::string("10Bit YUV (") + fmtChars + ")"; break;
        case bmdFormat10BitYUVA: formatName = std::string("10Bit YUVA (") + fmtChars + ")"; break;
        
        case bmdFormat8BitARGB: formatName = std::string("8Bit ARGB (") + std::to_string(format) + ")"; break;
        case bmdFormat8BitBGRA: formatName = std::string("8Bit BGRA (") + fmtChars + ")"; break;
        
        case bmdFormat10BitRGB: formatName = std::string("10Bit RGB (") + fmtChars + ")"; break;
        case bmdFormat12BitRGB: formatName = std::string("12Bit RGB (") + fmtChars + ")"; break;
        case bmdFormat12BitRGBLE: formatName = std::string("12Bit RGB LE (") + fmtChars + ")"; break;

        case bmdFormat10BitRGBXLE: formatName = std::string("10Bit RGBX LE (") + fmtChars + ")"; break;
        case bmdFormat10BitRGBX: formatName = std::string("10Bit RGBX (") + fmtChars + ")"; break;
        
        default: formatName = std::string("Unknown (") + fmtChars + ")"; break;
    }
    
    strncpy(name, formatName.c_str(), name_size - 1);
    name[name_size - 1] = '\0';
    return 0;
}

// Version info
const char* decklink_get_driver_version() {
    static std::string version;
    if (!version.empty()) return version.c_str();
    IDeckLinkAPIInformation* apiInfo = CreateDeckLinkAPIInformationInstance();
    if (apiInfo) {
        int64_t versionInt = 0;
        if (apiInfo->GetInt(BMDDeckLinkAPIVersion, &versionInt) == S_OK) {
            int major = (versionInt >> 24) & 0xFF;
            int minor = (versionInt >> 16) & 0xFF;
            int patch = (versionInt >> 8) & 0xFF;
            char buf[32];
            snprintf(buf, sizeof(buf), "%d.%d.%d", major, minor, patch);
            version = buf;
        } else {
            version = "unknown";
        }
        apiInfo->Release();
    } else {
        version = "unavailable";
    }
    return version.c_str();
}

const char* decklink_get_sdk_version() {
    static std::string sdk_version = BLACKMAGIC_DECKLINK_API_VERSION_STRING;
    return sdk_version.c_str();
}

// Add new C wrapper function for complete HDR metadata
int decklink_set_hdr_metadata(DeckLinkHandle handle, const HDRMetadata* metadata) {
    if (!handle || !metadata) return -1;
    auto* signalGen = static_cast<DeckLinkSignalGen*>(handle);
    return signalGen->setHDRMetadata(*metadata);
}

} // extern "C" 