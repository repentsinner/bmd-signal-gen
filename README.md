# BMD Signal Generator

This project enables the generation of test patterns in a cross-platform, highly deterministic way that is not perturbed by OS or GPU variability. It does this by providing C++ and Python wrappers for the BlackMagic Design Decklink API. Recommended output interfaces are the [UltraStudio 4K Mini](https://www.blackmagicdesign.com/products/ultrastudio/techspecs/W-DLUS-11) and the [UltraStudio Monitor 3G](https://www.blackmagicdesign.com/products/ultrastudio/techspecs/W-DLUS-13).
The project includes a basic command-line tool to illustrate how to use the wrapper.

## Overview

This project allows you to:
- Enumerate and select connected DeckLink devices
- Query supported pixel formats for each device
- Output solid RGB color test patterns to DeckLink devices
- Control output duration and device selection

## Requirements

- macOS (tested on macOS 15.5) or presumably Windows
- Blackmagic Design Desktop Video drivers (tested with 14.5)
- Blackmagic Design DeckLink SDK (tested with 14.4)
- Python 3.11+ (for Python interface)
- clang++ compiler

## Project Structure

```
bmd-signal-gen/
├── cpp/                          # C++ wrapper library
│   ├── decklink_wrapper.h        # C API header
│   ├── decklink_wrapper.cpp      # C API implementation
│   ├── pixel_packing.h           # Pixel format conversion utilities
│   ├── pixel_packing.cpp         # Pixel format conversion implementation
│   ├── PIXEL_PACKING_DOCUMENTATION.md  # Documentation for pixel packing
│   ├── Makefile                  # Build configuration for C++ library
│   └── Blackmagic DeckLink SDK 14.4/  # Blackmagic Design SDK
├── lib/                          # Python interface and built library
│   ├── bmd_decklink.py          # Python ctypes wrapper
│   └── libdecklink.dylib        # Built dynamic library (macOS)
├── bmd_signal_gen.py            # Command-line application
├── pyproject.toml               # Python project configuration
├── uv.lock                      # UV dependency lock file
├── .python-version              # Python version specification
├── .venv/                       # Python virtual environment
├── Makefile                     # Build configuration
└── README.md                    # This file
```

## Building

### Prerequisites

1. Install the Blackmagic Design Desktop Video driver
2. Install the Blackmagic Design DeckLink SDK
3. Ensure the SDK is located at `cpp/Blackmagic DeckLink SDK 14.4/` relative to the project root
4. Install Python 3.11+ and UV package manager

### Build the C++ Library

```bash
cd cpp
make clean
make
```

This creates `lib/libdecklink.dylib` - a dynamic library that provides the C API for DeckLink device control.

### Python Environment Setup

The project uses UV for Python dependency management:

```bash
# Install UV if not already installed
curl -LsSf https://astral.sh/uv/install.sh | sh

# Install dependencies and create virtual environment
uv sync
```

## Usage

### Python Command-Line Interface

The easiest way to use this project is through the Python command-line interface:

```bash
uv run bmd_signal_gen.py <r> <g> <b> [options]
```

#### Examples

Output red color for 5 seconds:
```bash
uv run bmd_signal_gen.py 255 0 0
```

#### Command-Line Options

- `r`, `g`, `b`: Red, green, blue components (0-255)
- `--duration`, `-d`: Output duration in seconds (default: 5.0)
- `--device`: Device index to use (default: 0)

### Python Library Usage

You can also use the Python wrapper directly in your own code:

```python
from bmd_decklink import BMDDeckLink, get_decklink_devices

# List available devices
devices = get_decklink_devices()
print("Available devices:", devices)

# Create device instance
decklink = BMDDeckLink(device_index=0)

# Get supported pixel formats
formats = decklink.get_supported_pixel_formats()
print("Supported formats:", formats)

# Output color
decklink.set_color(255, 0, 0)  # Red
decklink.start()
# ... wait ...
decklink.stop()
decklink.close()
```

### C++ Library Usage

The C++ library provides a C API that can be used from any language that supports C function calls:

```cpp
#include "cpp/decklink_wrapper.h"

// Get device count
int count = decklink_get_device_count();

// Open device
DeckLinkHandle handle = decklink_open_output_by_index(0);

// Set color and start output
decklink_set_color(handle, 255, 0, 0);
decklink_start_output(handle);

// ... wait ...

// Stop and close
decklink_stop_output(handle);
decklink_close(handle);
```

## API Reference

### C API Functions

- `int decklink_get_device_count()` - Get number of connected devices
- `int decklink_get_device_name_by_index(int index, char* name, int name_size)` - Get device name
- `DeckLinkHandle decklink_open_output_by_index(int index)` - Open device by index
- `void decklink_close(DeckLinkHandle handle)` - Close device
- `int decklink_set_color(DeckLinkHandle handle, int r, int g, int b)` - Set output color
- `int decklink_start_output(DeckLinkHandle handle)` - Start video output
- `int decklink_stop_output(DeckLinkHandle handle)` - Stop video output
- `int decklink_get_supported_pixel_format_count(DeckLinkHandle handle)` - Get pixel format count
- `int decklink_get_supported_pixel_format_name(DeckLinkHandle handle, int index, char* name, int name_size)` - Get pixel format name

### Python Classes

- `BMDDeckLink(device_index=0)` - Main device interface
- `get_decklink_devices()` - List available devices
- `get_supported_pixel_formats(handle)` - Get pixel formats for device

## Pixel Format Support

The project includes comprehensive pixel format conversion utilities in `cpp/pixel_packing.h` and `cpp/pixel_packing.cpp`. These utilities handle conversion between RGB color values and various DeckLink pixel formats including:

- 8-bit YUV (4:2:2)
- 10-bit YUV (4:2:2)
- 8-bit RGB
- 10-bit RGB
- 12-bit RGB

See `cpp/PIXEL_PACKING_DOCUMENTATION.md` for detailed information about supported formats and conversion algorithms.

## Troubleshooting

### No Devices Found
- Ensure DeckLink device is properly connected
- Check that Blackmagic Desktop Video software is installed
- Verify device drivers are up to date

### Build Errors
- Ensure DeckLink SDK is in the correct location
- Check that all required frameworks are available
- Verify compiler supports C++17

### Runtime Errors
- Check device permissions
- Ensure no other application is using the DeckLink device
- Verify the device supports video output

## License

This project is provided as-is for educational and development purposes. Please refer to the Blackmagic Design SDK license for DeckLink API usage terms.

## Contributing

Feel free to submit issues and enhancement requests. When contributing code, please ensure it compiles without warnings and follows the existing code style.



