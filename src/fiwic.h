/**
 * @file fiwic.h
 * @author Daniel Starke
 * @date 2024-12-03
 * @version 2026-10-26
 *
 * Public identity of the FreeImage WIC codec components.
 *
 * Every FreeImage format that is not handled natively by Windows gets its own
 * WIC decoder, WIC encoder and container format. Their GUIDs are derived from
 * the base GUIDs below and the FreeImage format index (see `formatGuid`).
 */
#ifndef __FIWIC_H__
#define __FIWIC_H__

#include <guiddef.h>


/** Registered ProgID of the codec. It holds the registration bookkeeping. */
#define FIWIC_PROGID L"fiwic.FreeImageWIC"

/** Friendly name of the codec. */
#define FIWIC_FRIENDLY_NAME L"FreeImage WIC Codec"

/** Vendor name reported to WIC. */
#define FIWIC_VENDOR_NAME L"Daniel Starke"

/** Vendor URL reported to WIC. */
#define FIWIC_VENDOR_URL L"https://github.com/daniel-starke/freeimage-wic"

/** Component version reported to WIC (dotted quad from the build, see common.mk). */
#define FIWIC_WIDEN_(x) L ## x
#define FIWIC_WIDEN(x) FIWIC_WIDEN_(x)
#define FIWIC_REG_VERSION FIWIC_WIDEN(FIWIC_VERSION_QUAD)


/*
 * {7C2B9A45-1D6E-4F83-B0A7-53E9C482D61B}
 *
 * Base of the decoder class IDs. The class ID of the decoder for the
 * FreeImage format `fif` is this value with `fif + 1` added to `Data1`.
 */
DEFINE_GUID(CLSID_FIWIC_BaseDecoder,
	0x7c2b9a45, 0x1d6e, 0x4f83, 0xb0, 0xa7, 0x53, 0xe9, 0xc4, 0x82, 0xd6, 0x1b);

/*
 * {3F9D6B72-8A14-4C05-95E8-2D47B3A61C90}
 *
 * Base of the encoder class IDs, derived like the decoder class IDs.
 */
DEFINE_GUID(CLSID_FIWIC_BaseEncoder,
	0x3f9d6b72, 0x8a14, 0x4c05, 0x95, 0xe8, 0x2d, 0x47, 0xb3, 0xa6, 0x1c, 0x90);

/*
 * {D4E7F109-9C25-46B8-A3D0-71F62E8C5B94}
 *
 * Base of the container format GUIDs, derived like the decoder class IDs.
 */
DEFINE_GUID(GUID_FIWIC_BaseContainerFormat,
	0xd4e7f109, 0x9c25, 0x46b8, 0xa3, 0xd0, 0x71, 0xf6, 0x2e, 0x8c, 0x5b, 0x94);

/*
 * {8E5F3C21-7A40-4B92-9D18-4C6B0E2A7F55}
 *
 * Vendor of all components of this codec.
 */
DEFINE_GUID(GUID_Vendor_FIWIC,
	0x8e5f3c21, 0x7a40, 0x4b92, 0x9d, 0x18, 0x4c, 0x6b, 0x0e, 0x2a, 0x7f, 0x55);


#endif /* __FIWIC_H__ */
