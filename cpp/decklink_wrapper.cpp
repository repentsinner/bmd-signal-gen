#include "decklink_wrapper.h"
#include "pixel_packing.h"
#include <iostream>
#include <algorithm>
#include <cstring>
#include "DeckLinkAPIVersion.h"
#include <CoreFoundation/CoreFoundation.h>

// DeckLinkSignalGen Implementation
DeckLinkSignalGen::DeckLinkSignalGen() 
    : m_device(nullptr)
    , m_output(nullptr)
    , m_frame(nullptr)
    , m_width(1920)
    , m_height(1080)
    , m_outputEnabled(false)
    , m_pixelFormat(bmdFormat12BitRGB)
    , m_eotfType(-1)
    , m_maxCLL(2000)
    , m_maxFALL(400)
    , m_formatsCached(false) {
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
        int32_t width = m_frame->GetWidth();
        int32_t height = m_frame->GetHeight();
        int32_t rowBytes = m_frame->GetRowBytes();
        BMDPixelFormat format = m_frame->GetPixelFormat();
        
        std::cerr << "[DeckLink] Frame info " << context << ":" << std::endl;
        std::cerr << "  Width: " << std::dec << width << ", Height: " << std::dec << height << std::endl;
        std::cerr << "  RowBytes: " << std::dec << rowBytes << std::endl;
        std::cerr << "  PixelFormat: 0x" << std::hex << format << std::dec << std::endl;
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
              << " pixelFormat=0x" << std::hex << m_pixelFormat << std::dec
              << ", width=" << m_width
              << ", height=" << m_height << std::endl;
    HRESULT result = m_output->RowBytesForPixelFormat(m_pixelFormat, m_width, &rowBytes);
    if (result != S_OK) {
        std::cerr << "[DeckLink] RowBytesForPixelFormat failed. HRESULT: 0x" << std::hex << result << std::dec << std::endl;
        std::cerr << "[DeckLink][DEBUG] Arguments were: pixelFormat=0x" << std::hex << m_pixelFormat << std::dec
                  << ", width=" << m_width
                  << ", height=" << m_height << std::endl;
        return -3;
    }
    // Output rowBytes in decimal notation
    std::cerr << "[DeckLink][DEBUG] RowBytesForPixelFormat returned rowBytes=" << rowBytes << std::endl;
    
    result = m_output->CreateVideoFrame(m_width, m_height, rowBytes, m_pixelFormat, bmdFrameFlagDefault, &m_frame);
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
    
    // Extract RGB channels from the raw data (3 uint16_t per pixel: R, G, B)
    std::vector<uint16_t> r_channel(m_width * m_height);
    std::vector<uint16_t> g_channel(m_width * m_height);
    std::vector<uint16_t> b_channel(m_width * m_height);
    
    for (int i = 0; i < m_width * m_height; i++) {
        r_channel[i] = srcData[i * 3 + 0];
        g_channel[i] = srcData[i * 3 + 1];
        b_channel[i] = srcData[i * 3 + 2];
    }
    
    // Pack the data according to the pixel format
    switch (m_pixelFormat) {
        case bmdFormat8BitBGRA: {
            // Convert 16-bit to 8-bit
            std::vector<uint8_t> r_8bit(m_width * m_height);
            std::vector<uint8_t> g_8bit(m_width * m_height);
            std::vector<uint8_t> b_8bit(m_width * m_height);
            
            for (int i = 0; i < m_width * m_height; i++) {
                r_8bit[i] = (r_channel[i] * 255) / 65535;
                g_8bit[i] = (g_channel[i] * 255) / 65535;
                b_8bit[i] = (b_channel[i] * 255) / 65535;
            }
            
            pack_8bit_rgb_image(frameData, r_8bit.data(), g_8bit.data(), b_8bit.data(),
                              m_width, m_height, rowBytes, true);
            break;
        }
        case bmdFormat8BitARGB: {
            // Convert 16-bit to 8-bit
            std::vector<uint8_t> r_8bit(m_width * m_height);
            std::vector<uint8_t> g_8bit(m_width * m_height);
            std::vector<uint8_t> b_8bit(m_width * m_height);
            
            for (int i = 0; i < m_width * m_height; i++) {
                r_8bit[i] = (r_channel[i] * 255) / 65535;
                g_8bit[i] = (g_channel[i] * 255) / 65535;
                b_8bit[i] = (b_channel[i] * 255) / 65535;
            }
            
            pack_8bit_rgb_image(frameData, r_8bit.data(), g_8bit.data(), b_8bit.data(),
                              m_width, m_height, rowBytes, false);
            break;
        }
        case bmdFormat10BitRGB: {
            // Convert 16-bit to 10-bit
            std::vector<uint16_t> r_10bit(m_width * m_height);
            std::vector<uint16_t> g_10bit(m_width * m_height);
            std::vector<uint16_t> b_10bit(m_width * m_height);
            
            for (int i = 0; i < m_width * m_height; i++) {
                r_10bit[i] = (r_channel[i] * 1023) / 65535;
                g_10bit[i] = (g_channel[i] * 1023) / 65535;
                b_10bit[i] = (b_channel[i] * 1023) / 65535;
            }
            
            pack_10bit_rgb_image(frameData, r_10bit.data(), g_10bit.data(), b_10bit.data(),
                               m_width, m_height, rowBytes);
            break;
        }
        case bmdFormat8BitYUV: {
            // Convert RGB to YUV
            std::vector<uint16_t> y_channel(m_width * m_height);
            std::vector<uint16_t> u_channel(m_width * m_height);
            std::vector<uint16_t> v_channel(m_width * m_height);
            
            for (int i = 0; i < m_width * m_height; i++) {
                // Convert RGB to YUV (simplified conversion)
                uint16_t r = r_channel[i];
                uint16_t g = g_channel[i];
                uint16_t b = b_channel[i];
                
                y_channel[i] = (66 * r + 129 * g + 25 * b + 128) >> 8;
                u_channel[i] = (-38 * r - 74 * g + 112 * b + 128) >> 8;
                v_channel[i] = (112 * r - 94 * g - 18 * b + 128) >> 8;
                
                // Clamp values
                y_channel[i] = std::min(std::max(y_channel[i], (uint16_t)0), (uint16_t)255);
                u_channel[i] = std::min(std::max(u_channel[i], (uint16_t)0), (uint16_t)255);
                v_channel[i] = std::min(std::max(v_channel[i], (uint16_t)0), (uint16_t)255);
            }
            
            pack_10bit_yuv_image(frameData, y_channel.data(), u_channel.data(), v_channel.data(),
                               m_width, m_height, rowBytes);
            break;
        }
        case bmdFormat12BitRGB: {
            // Convert 16-bit to 12-bit
            std::vector<uint16_t> r_12bit(m_width * m_height);
            std::vector<uint16_t> g_12bit(m_width * m_height);
            std::vector<uint16_t> b_12bit(m_width * m_height);
            
            for (int i = 0; i < m_width * m_height; i++) {
                r_12bit[i] = (r_channel[i] * 4095) / 65535;
                g_12bit[i] = (g_channel[i] * 4095) / 65535;
                b_12bit[i] = (b_channel[i] * 4095) / 65535;
            }
            
            pack_12bit_rgb_image(frameData, r_12bit.data(), g_12bit.data(), b_12bit.data(),
                               m_width, m_height, rowBytes);
            break;
        }
        default:
            std::cerr << "[DeckLink] Unsupported pixel format: 0x" << std::hex << m_pixelFormat << std::dec << std::endl;
            videoBuffer->EndAccess(bmdBufferAccessWrite);
            videoBuffer->Release();
            return -8;
    }
    
    videoBuffer->EndAccess(bmdBufferAccessWrite);
    videoBuffer->Release();
    
    // Apply EOTF metadata if set
    if (m_eotfType >= 0) {
        applyEOTFMetadata();
    }
    
    logFrameInfo("created");
    return 0;
}

int DeckLinkSignalGen::scheduleFrame() {
    if (!m_output || !m_frame) return -1;
    
    // Schedule the frame for output
    HRESULT result = m_output->ScheduleVideoFrame(m_frame, 0, 1000, 30000);
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
    
    // Cache supported formats if not already done
    if (!m_formatsCached) {
        cacheSupportedFormats();
    }
    
    if (pixelFormatIndex < 0 || pixelFormatIndex >= static_cast<int>(m_supportedFormats.size())) {
        std::cerr << "[DeckLink] Invalid pixel format index: " << pixelFormatIndex << std::endl;
        return -1;
    }
    
    m_pixelFormat = m_supportedFormats[pixelFormatIndex];
    std::cerr << "[DeckLink] Set pixel format to index " << pixelFormatIndex 
              << " (0x" << std::hex << m_pixelFormat << std::dec << ")" << std::endl;
    
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

int DeckLinkSignalGen::setEOTFMetadata(int eotf, uint16_t maxCLL, uint16_t maxFALL) {
    m_eotfType = eotf;
    m_maxCLL = maxCLL;
    m_maxFALL = maxFALL;
    
    std::cerr << "[DeckLink] Set EOTF metadata - EOTF: " << eotf 
              << ", MaxCLL: " << maxCLL << ", MaxFALL: " << maxFALL << std::endl;
    
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

int DeckLinkSignalGen::applyEOTFMetadata() {
    if (!m_frame || m_eotfType < 0) return 0;
    
    // Get the metadata extensions interface
    IDeckLinkVideoFrameMutableMetadataExtensions* metadataExt = nullptr;
    HRESULT result = m_frame->QueryInterface(IID_IDeckLinkVideoFrameMutableMetadataExtensions, (void**)&metadataExt);
    if (result != S_OK || !metadataExt) return -1;
    
    // Set EOTF metadata (0-7 as per CEA 861.3)
    if (m_eotfType >= 0 && m_eotfType <= 7) {
        metadataExt->SetInt(bmdDeckLinkFrameMetadataHDRElectroOpticalTransferFunc, m_eotfType);
    }
    
    // Set HDR metadata if provided
    if (m_maxCLL > 0) {
        metadataExt->SetFloat(bmdDeckLinkFrameMetadataHDRMaximumContentLightLevel, static_cast<double>(m_maxCLL));
    }
    if (m_maxFALL > 0) {
        metadataExt->SetFloat(bmdDeckLinkFrameMetadataHDRMaximumFrameAverageLightLevel, static_cast<double>(m_maxFALL));
    }
    
    // Set the HDR metadata flag
    BMDFrameFlags currentFlags = m_frame->GetFlags();
    m_frame->SetFlags(currentFlags | bmdFrameContainsHDRMetadata);
    
    metadataExt->Release();
    return 0;
}

// Thin C wrapper implementation
extern "C" {

int decklink_set_eotf_metadata(DeckLinkHandle handle, int eotf, uint16_t maxCLL, uint16_t maxFALL) {
    if (!handle) return -1;
    auto* signalGen = static_cast<DeckLinkSignalGen*>(handle);
    return signalGen->setEOTFMetadata(eotf, maxCLL, maxFALL);
}

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
    // Interpret format as 4 ASCII characters
    char fmtChars[5] = {
        static_cast<char>((format >> 24) & 0xFF),
        static_cast<char>((format >> 16) & 0xFF),
        static_cast<char>((format >> 8) & 0xFF),
        static_cast<char>(format & 0xFF),
        '\0'
    };
    std::string formatName;
    
    switch (format) {
        case bmdFormat8BitYUV: formatName = std::string("8-bit YUV (") + fmtChars + ")"; break;
        case bmdFormat10BitYUV: formatName = std::string("10-bit YUV (") + fmtChars + ")"; break;
        case bmdFormat10BitYUVA: formatName = std::string("10-bit YUVA (") + fmtChars + ")"; break;
        
        case bmdFormat8BitARGB: formatName = std::string("8-bit ARGB (") + std::to_string(format) + ")"; break;
        case bmdFormat8BitBGRA: formatName = std::string("8-bit BGRA (") + fmtChars + ")"; break;
        
        case bmdFormat10BitRGB: formatName = std::string("10-bit RGB (") + fmtChars + ")"; break;
        case bmdFormat12BitRGB: formatName = std::string("12-bit RGB (") + fmtChars + ")"; break;
        case bmdFormat12BitRGBLE: formatName = std::string("12-bit RGB LE (") + fmtChars + ")"; break;

        case bmdFormat10BitRGBXLE: formatName = std::string("10-bit RGBX LE (") + fmtChars + ")"; break;
        case bmdFormat10BitRGBX: formatName = std::string("10-bit RGBX (") + fmtChars + ")"; break;
        
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

} // extern "C" 