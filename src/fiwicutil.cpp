/**
 * @file fiwicutil.cpp
 * @author Daniel Starke
 * @date 2024-12-03
 * @version 2026-10-26
 *
 * Shared infrastructure of the FreeImage WIC codec.
 */
#include <string.h>
#include <wchar.h>
#include <windows.h>
#include <objbase.h>
#include <wincodec.h>
/* Defines the GUIDs declared by fiwic.h in this translation unit. */
#include <initguid.h>
#include <string>
#include <vector>
#include <FreeImage.h>
#include <fiwic.h>
#include <fiwicutil.hpp>


/* module state ***************************************************************/


/** Number of live COM objects created by this module. */
static LONG gObjectCount = 0;

/** Number of `IClassFactory::LockServer` locks. */
static LONG gLockCount = 0;

/** `1` once FreeImage has been initialized. */
static volatile LONG gFreeImageReady = 0;

/** Lock guarding the FreeImage initialization. */
static CriticalSection gFreeImageLock;

/** HMODULE of this DLL. */
static HMODULE gModule = nullptr;


extern "C" BOOL WINAPI dllMain(HINSTANCE hInstance, DWORD reason, LPVOID /* reserved */) noexcept {
	switch (reason) {
	case DLL_PROCESS_ATTACH:
		gModule = HMODULE(hInstance);
		DisableThreadLibraryCalls(HMODULE(hInstance));
		break;
	case DLL_PROCESS_DETACH:
		/* FreeImage is not torn down here. Its plug-in teardown must not run under
		 * the loader lock and FreeImage.dll shuts itself down when it is unloaded. */
		break;
	default:
		break;
	}
	return TRUE;
}


HMODULE moduleHandle() noexcept {
	return gModule;
}


bool ensureFreeImage() noexcept {
	Guard guard(gFreeImageLock);
	if (gFreeImageReady == 0) {
		FreeImage_Initialise(FALSE);
		InterlockedExchange(&gFreeImageReady, 1);
	}
	return FreeImage_GetFIFCount() > 0;
}


void objectCreated() noexcept {
	InterlockedIncrement(&gObjectCount);
}


void objectDestroyed() noexcept {
	InterlockedDecrement(&gObjectCount);
}


LONG objectCount() noexcept {
	return InterlockedCompareExchange(&gObjectCount, 0, 0);
}


void lockServer(bool lock) noexcept {
	if ( lock ) {
		InterlockedIncrement(&gLockCount);
		return;
	}
	/* An unbalanced unlock must not leave the count negative. */
	for (;;) {
		const LONG current = InterlockedCompareExchange(&gLockCount, 0, 0);
		if (current <= 0) {
			return;
		}
		if (InterlockedCompareExchange(&gLockCount, current - 1, current) == current) {
			return;
		}
	}
}


LONG lockCount() noexcept {
	return InterlockedCompareExchange(&gLockCount, 0, 0);
}


/* formats ********************************************************************/


/**
 * FreeImage formats that Windows decodes and encodes by itself.
 * JPEG XR, WebP and RAW are included since Windows offers them as part of
 * the system or its image extensions.
 */
static const FREE_IMAGE_FORMAT nativeFormats[] = {
	FIF_BMP,
	FIF_ICO,
	FIF_JPEG,
	FIF_PNG,
	FIF_TIFF,
	FIF_GIF,
	FIF_DDS,
	FIF_JXR,
	FIF_WEBP,
	FIF_RAW
};


/** FreeImage formats which the multi page API supports. */
static const FREE_IMAGE_FORMAT multiPageFormats[] = {
	FIF_GIF,
	FIF_ICO,
	FIF_TIFF
};


/** File signature of a FreeImage format. */
struct SignatureRow {
	FREE_IMAGE_FORMAT fif; /**< FreeImage format. */
	DWORD position; /**< Offset of the signature. */
	const char * pattern; /**< Hexadecimal bytes. */
	const char * mask; /**< Hexadecimal compare mask (`nullptr` to compare everything). */
};


/**
 * Signatures of the non native formats.
 * Formats without an entry (Koala, CUT, FAX G3, PICT, Targa and WBMP) have no
 * reliable signature.
 */
static const SignatureRow signatures[] = {
	{FIF_JNG, 0, "8B 4A 4E 47 0D 0A 1A 0A", nullptr},
	{FIF_LBM, 0, "46 4F 52 4D 00 00 00 00 49 4C 42 4D", "FF FF FF FF 00 00 00 00 FF FF FF FF"},
	{FIF_MNG, 0, "8A 4D 4E 47 0D 0A 1A 0A", nullptr},
	{FIF_PBM, 0, "50 31", nullptr},
	{FIF_PBMRAW, 0, "50 34", nullptr},
	{FIF_PCD, 2048, "50 43 44 5F 49 50 49", nullptr},
	{FIF_PCX, 0, "0A 00", "FF 00"},
	{FIF_PGM, 0, "50 32", nullptr},
	{FIF_PGMRAW, 0, "50 35", nullptr},
	{FIF_PPM, 0, "50 33", nullptr},
	{FIF_PPMRAW, 0, "50 36", nullptr},
	{FIF_RAS, 0, "59 A6 6A 95", nullptr},
	{FIF_PSD, 0, "38 42 50 53", nullptr},
	{FIF_XBM, 0, "23 64 65 66 69 6E 65", nullptr},
	{FIF_XPM, 0, "2F 2A 20 58 50 4D 20 2A 2F", nullptr},
	{FIF_HDR, 0, "23 3F", nullptr},
	{FIF_SGI, 0, "01 DA", nullptr},
	{FIF_EXR, 0, "76 2F 31 01", nullptr},
	{FIF_J2K, 0, "FF 4F FF 51", nullptr},
	{FIF_JP2, 0, "00 00 00 0C 6A 50 20 20 0D 0A 87 0A", nullptr},
	{FIF_PFM, 0, "50 46", nullptr},
	{FIF_PFM, 0, "50 66", nullptr}
};


/**
 * Parses a string of hexadecimal bytes.
 *
 * @param[in] hex - bytes separated by blanks
 * @return parsed bytes
 */
static std::vector<BYTE> parseHexBytes(const char * hex) {
	std::vector<BYTE> res;
	unsigned value = 0;
	bool inByte = false;
	for (const char * p = hex; ; ++p) {
		unsigned digit = 0;
		if (*p >= '0' && *p <= '9') {
			digit = unsigned(*p - '0');
		} else if (*p >= 'A' && *p <= 'F') {
			digit = unsigned(*p - 'A') + 10;
		} else if (*p >= 'a' && *p <= 'f') {
			digit = unsigned(*p - 'a') + 10;
		} else {
			if ( inByte ) {
				res.push_back(BYTE(value));
				value = 0;
				inByte = false;
			}
			if (*p == '\0') {
				break;
			}
			continue;
		}
		value = (value << 4) | digit;
		inByte = true;
	}
	return res;
}


bool isNativeFormat(FREE_IMAGE_FORMAT fif) noexcept {
	for (size_t i = 0; i < (sizeof(nativeFormats) / sizeof(nativeFormats[0])); ++i) {
		if (nativeFormats[i] == fif) {
			return true;
		}
	}
	return false;
}


bool isMultiPageFormat(FREE_IMAGE_FORMAT fif) noexcept {
	for (size_t i = 0; i < (sizeof(multiPageFormats) / sizeof(multiPageFormats[0])); ++i) {
		if (multiPageFormats[i] == fif) {
			return true;
		}
	}
	return false;
}


bool isDecoderFormat(FREE_IMAGE_FORMAT fif) noexcept {
	ensureFreeImage();
	if (fif < 0 || fif >= FreeImage_GetFIFCount() || isNativeFormat(fif)) {
		return false;
	}
	return FreeImage_IsPluginEnabled(fif) > 0 && FreeImage_FIFSupportsReading(fif);
}


bool isEncoderFormat(FREE_IMAGE_FORMAT fif) noexcept {
	ensureFreeImage();
	if (fif < 0 || fif >= FreeImage_GetFIFCount() || isNativeFormat(fif)) {
		return false;
	}
	return FreeImage_IsPluginEnabled(fif) > 0 && FreeImage_FIFSupportsWriting(fif);
}


/**
 * Returns the base GUID of a component kind.
 *
 * @param[in] kind - component kind
 * @return base GUID
 */
static const GUID & baseGuid(GuidKind kind) noexcept {
	switch (kind) {
	case FIWIC_GUID_DECODER: return CLSID_FIWIC_BaseDecoder;
	case FIWIC_GUID_ENCODER: return CLSID_FIWIC_BaseEncoder;
	default: break;
	}
	return GUID_FIWIC_BaseContainerFormat;
}


/**
 * Offset of the class IDs of decoders without a file signature.
 * Such decoders match every stream. Sorting their class IDs behind all others
 * lets WIC try the decoders with a signature first. Targa, the most common of
 * these formats, takes the first place among them (offset `0x100` itself).
 */
static const unsigned long kNoSignatureOffset = 0x100;


/**
 * Checks if a FreeImage format has a file signature.
 *
 * @param[in] fif - FreeImage format
 * @return `true` if `signatures` has an entry for the format
 */
static bool hasSignature(FREE_IMAGE_FORMAT fif) noexcept {
	for (size_t i = 0; i < (sizeof(signatures) / sizeof(signatures[0])); ++i) {
		if (signatures[i].fif == fif) {
			return true;
		}
	}
	return false;
}


GUID formatGuid(GuidKind kind, FREE_IMAGE_FORMAT fif) noexcept {
	GUID res = baseGuid(kind);
	res.Data1 += static_cast<unsigned long>(int(fif) + 1);
	if (kind == FIWIC_GUID_DECODER && ( ! hasSignature(fif) )) {
		res.Data1 = baseGuid(kind).Data1 + ((fif == FIF_TARGA) ? kNoSignatureOffset : (kNoSignatureOffset + static_cast<unsigned long>(int(fif) + 1)));
	}
	return res;
}


bool parseFormatGuid(GuidKind kind, const GUID & guid, FREE_IMAGE_FORMAT * fif) noexcept {
	const GUID & base = baseGuid(kind);
	if (guid.Data2 != base.Data2 || guid.Data3 != base.Data3 || memcmp(guid.Data4, base.Data4, sizeof(base.Data4)) != 0) {
		return false;
	}
	if (guid.Data1 <= base.Data1 || (guid.Data1 - base.Data1) >= (2 * kNoSignatureOffset)) {
		return false;
	}
	if (kind == FIWIC_GUID_DECODER && (guid.Data1 - base.Data1) == kNoSignatureOffset) {
		*fif = FIF_TARGA;
		return true;
	}
	unsigned long index = guid.Data1 - base.Data1 - 1;
	const bool late = index >= kNoSignatureOffset;
	if ( late ) {
		index -= kNoSignatureOffset;
	}
	if (index >= 0xFF) {
		return false;
	}
	*fif = FREE_IMAGE_FORMAT(index);
	/* Reject GUIDs which formatGuid would not have created. */
	return late == (kind == FIWIC_GUID_DECODER && ( ! hasSignature(*fif) ));
}


std::vector<Pattern> formatPatterns(FREE_IMAGE_FORMAT fif) {
	std::vector<Pattern> res;
	for (size_t i = 0; i < (sizeof(signatures) / sizeof(signatures[0])); ++i) {
		if (signatures[i].fif != fif) {
			continue;
		}
		Pattern pattern;
		pattern.position = signatures[i].position;
		pattern.pattern = parseHexBytes(signatures[i].pattern);
		if (signatures[i].mask != nullptr) {
			pattern.mask = parseHexBytes(signatures[i].mask);
		} else {
			pattern.mask.assign(pattern.pattern.size(), 0xFF);
		}
		res.push_back(pattern);
	}
	if ( res.empty() ) {
		/* Candidate for every stream; QueryCapability makes the decision. */
		Pattern any;
		any.position = 0;
		any.pattern.assign(1, 0x00);
		any.mask.assign(1, 0x00);
		res.push_back(any);
	}
	return res;
}


/* strings and extensions *****************************************************/


std::wstring toWide(const char * s) {
	std::wstring res;
	if (s == nullptr) {
		return res;
	}
	const int len = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
	if (len > 0) {
		res.resize(size_t(len) - 1);
		MultiByteToWideChar(CP_UTF8, 0, s, -1, &res[0], len);
	}
	return res;
}


void upperCase(std::wstring & s) noexcept {
	for (size_t i = 0; i < s.size(); ++i) {
		if (s[i] >= L'a' && s[i] <= L'z') {
			s[i] = wchar_t(s[i] - (L'a' - L'A'));
		}
	}
}


std::wstring fifExtensions(FREE_IMAGE_FORMAT fif, const wchar_t * prefix, wchar_t sep) {
	std::wstring res;
	const std::wstring list = toWide(FreeImage_GetFIFExtensionList(fif));
	size_t pos = 0;
	while (pos <= list.size()) {
		size_t end = list.find(L',', pos);
		if (end == std::wstring::npos) {
			end = list.size();
		}
		if (end > pos) {
			if ( ! res.empty() ) {
				res += sep;
			}
			res += prefix;
			res.append(list, pos, end - pos);
		}
		pos = end + 1;
	}
	return res;
}


bool extensionMatches(const wchar_t * extensions, const std::wstring & fileName) {
	if (extensions == nullptr) {
		return false;
	}
	const size_t dot = fileName.find_last_of(L'.');
	const size_t sep = fileName.find_last_of(L"\\/");
	if (dot == std::wstring::npos || (dot + 1) >= fileName.size()) {
		return false;
	}
	if (sep != std::wstring::npos && sep > dot) {
		return false;
	}
	std::wstring ext(fileName, dot + 1);
	upperCase(ext);
	const size_t len = wcslen(extensions);
	size_t pos = 0;
	while (pos <= len) {
		size_t end = pos;
		while (end < len && extensions[end] != L';' && extensions[end] != L',') {
			++end;
		}
		std::wstring item(extensions, pos, end - pos);
		/* Drop an optional `*.` or `.` prefix. */
		if (( ! item.empty() ) && item[0] == L'*') {
			item.erase(0, 1);
		}
		if (( ! item.empty() ) && item[0] == L'.') {
			item.erase(0, 1);
		}
		upperCase(item);
		if (( ! item.empty() ) && item == ext) {
			return true;
		}
		pos = end + 1;
	}
	return false;
}


/* pixel formats **************************************************************/


/** Pixel formats used by the codec. The fallback entries index this table. */
static const PixelInfo pixelInfo[] = {
	/*  0 */ {&GUID_WICPixelFormat1bppIndexed, FIT_BITMAP, 1, true, {0, 2, 4, 5, -1, -1, -1, -1}},
	/*  1 */ {&GUID_WICPixelFormat4bppIndexed, FIT_BITMAP, 4, true, {1, 2, 4, 5, -1, -1, -1, -1}},
	/*  2 */ {&GUID_WICPixelFormat8bppIndexed, FIT_BITMAP, 8, true, {2, 4, 5, -1, -1, -1, -1, -1}},
	/*  3 */ {&GUID_WICPixelFormat8bppGray, FIT_BITMAP, 8, false, {3, 4, 5, 10, 2, 0, -1, -1}},
	/*  4 */ {&GUID_WICPixelFormat24bppBGR, FIT_BITMAP, 24, false, {4, 5, 3, 10, 2, 0, -1, -1}},
	/*  5 */ {&GUID_WICPixelFormat32bppBGRA, FIT_BITMAP, 32, false, {5, 4, 3, 10, 2, 0, -1, -1}},
	/*  6 */ {&GUID_WICPixelFormat16bppGray, FIT_UINT16, 16, false, {6, 3, 4, 5, 10, -1, -1, -1}},
	/*  7 */ {&GUID_WICPixelFormat48bppRGB, FIT_RGB16, 48, false, {7, 4, 5, 10, -1, -1, -1, -1}},
	/*  8 */ {&GUID_WICPixelFormat64bppRGBA, FIT_RGBA16, 64, false, {8, 5, 4, 10, -1, -1, -1, -1}},
	/*  9 */ {&GUID_WICPixelFormat32bppGrayFloat, FIT_FLOAT, 32, false, {9, 6, 3, 4, 5, 10, -1, -1}},
	/* 10 */ {&GUID_WICPixelFormat128bppRGBAFloat, FIT_RGBAF, 128, false, {10, 8, 7, 5, 4, -1, -1, -1}}
};


/** Substitutes for pixel formats unknown to the codec. */
static const int defaultPixelFallback[] = {5, 4, 3, 10, 2, 0, -1};


const PixelInfo * findPixelInfo(const GUID & format) noexcept {
	for (size_t i = 0; i < (sizeof(pixelInfo) / sizeof(pixelInfo[0])); ++i) {
		if ( IsEqualGUID(*pixelInfo[i].format, format) ) {
			return &pixelInfo[i];
		}
	}
	return nullptr;
}


const PixelInfo * pixelInfoAt(size_t index) noexcept {
	return (index < sizeof(pixelInfo) / sizeof(pixelInfo[0])) ? &pixelInfo[index] : nullptr;
}


const PixelInfo * negotiatePixelFormat(FREE_IMAGE_FORMAT fif, const GUID & requested) noexcept {
	const PixelInfo * start = findPixelInfo(requested);
	const int * chain = (start != nullptr) ? start->fallback : defaultPixelFallback;
	for (size_t i = 0; chain[i] >= 0; ++i) {
		const PixelInfo & info = pixelInfo[chain[i]];
		bool supported;
		if (info.type == FIT_BITMAP) {
			supported = FreeImage_FIFSupportsExportBPP(fif, int(info.bpp)) != FALSE;
		} else if (info.type == FIT_RGBAF) {
			/* The frame encoder drops the alpha channel for RGB only formats. */
			supported = FreeImage_FIFSupportsExportType(fif, FIT_RGBAF) != FALSE || FreeImage_FIFSupportsExportType(fif, FIT_RGBF) != FALSE;
		} else {
			supported = FreeImage_FIFSupportsExportType(fif, info.type) != FALSE;
		}
		if ( supported ) {
			return &info;
		}
	}
	return nullptr;
}


/* streams ********************************************************************/


/**
 * Largest stream (in bytes) that is handled. The FreeImage stream interface
 * addresses it with a signed 32 bit offset.
 */
static const ULONGLONG maxStreamSize = 0x7FFFFFFFull;


/**
 * Limits the item count so that `size * count` fits in 32 bits.
 *
 * @param[in] size - item size in bytes (not zero)
 * @param[in] count - requested item count
 * @return usable item count
 */
static unsigned limitCount(unsigned size, unsigned count) noexcept {
	const unsigned long long total = static_cast<unsigned long long>(size) * count;
	return (total > 0xFFFFFFFFull) ? unsigned(0xFFFFFFFFull / size) : count;
}


/** `FreeImageIO::read_proc` on an `IStream`. */
static unsigned DLL_CALLCONV ioRead(void * buffer, unsigned size, unsigned count, fi_handle handle) {
	if (size == 0 || count == 0) {
		return 0;
	}
	count = limitCount(size, count);
	ULONG got = 0;
	const HRESULT hr = static_cast<IStream *>(handle)->Read(buffer, size * count, &got);
	return SUCCEEDED(hr) ? (got / size) : 0;
}


/** `FreeImageIO::write_proc` on an `IStream`. */
static unsigned DLL_CALLCONV ioWrite(void * buffer, unsigned size, unsigned count, fi_handle handle) {
	if (size == 0 || count == 0) {
		return 0;
	}
	count = limitCount(size, count);
	ULONG written = 0;
	const HRESULT hr = static_cast<IStream *>(handle)->Write(buffer, size * count, &written);
	return SUCCEEDED(hr) ? (written / size) : 0;
}


/** `FreeImageIO::seek_proc` on an `IStream`. The origins share their values. */
static int DLL_CALLCONV ioSeek(fi_handle handle, long offset, int origin) {
	LARGE_INTEGER move;
	move.QuadPart = offset;
	return SUCCEEDED(static_cast<IStream *>(handle)->Seek(move, DWORD(origin), nullptr)) ? 0 : -1;
}


/** `FreeImageIO::tell_proc` on an `IStream`. */
static long DLL_CALLCONV ioTell(fi_handle handle) {
	LARGE_INTEGER zero;
	zero.QuadPart = 0;
	ULARGE_INTEGER pos;
	pos.QuadPart = 0;
	if ( FAILED(static_cast<IStream *>(handle)->Seek(zero, STREAM_SEEK_CUR, &pos)) ) {
		return -1;
	}
	if (pos.QuadPart > maxStreamSize) {
		return -1;
	}
	return long(pos.QuadPart);
}


void initIo(FreeImageIO * io) noexcept {
	io->read_proc = ioRead;
	io->write_proc = ioWrite;
	io->seek_proc = ioSeek;
	io->tell_proc = ioTell;
}


HRESULT streamAsMemory(IStream * stream, ComPtr<IStream> & out) {
	if (stream == nullptr) {
		return E_POINTER;
	}
	ComPtr<IStream> mem;
	HRESULT hr = CreateStreamOnHGlobal(nullptr, TRUE, mem.GetAddressOf());
	if ( FAILED(hr) ) {
		return hr;
	}
	LARGE_INTEGER zero;
	zero.QuadPart = 0;
	hr = stream->Seek(zero, STREAM_SEEK_SET, nullptr);
	if ( FAILED(hr) ) {
		return hr;
	}
	/* `IStream::CopyTo` is not used because the file stream of WIC 1.0
	 * (Windows XP, Vista) does not implement it. */
	std::vector<BYTE> buffer(0x10000);
	ULONGLONG size = 0;
	for (;;) {
		ULONG got = 0;
		hr = stream->Read(&buffer[0], ULONG(buffer.size()), &got);
		if ( FAILED(hr) ) {
			return hr;
		}
		if (got == 0) {
			break;
		}
		size += got;
		/* Reading past the limit tells an oversized stream from one at the limit. */
		if (size > maxStreamSize) {
			return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
		}
		ULONG written = 0;
		hr = mem->Write(&buffer[0], got, &written);
		if ( FAILED(hr) ) {
			return hr;
		}
		if (written != got) {
			return STG_E_MEDIUMFULL;
		}
	}
	hr = mem->Seek(zero, STREAM_SEEK_SET, nullptr);
	if ( FAILED(hr) ) {
		return hr;
	}
	out = mem;
	return S_OK;
}


bool validateStream(FREE_IMAGE_FORMAT fif, IStream * stream) {
	if (stream == nullptr) {
		return false;
	}
	LARGE_INTEGER zero;
	zero.QuadPart = 0;
	ULARGE_INTEGER origin;
	origin.QuadPart = 0;
	if ( FAILED(stream->Seek(zero, STREAM_SEEK_CUR, &origin)) ) {
		return false;
	}
	LARGE_INTEGER back;
	back.QuadPart = LONGLONG(origin.QuadPart);
	FreeImageIO io;
	initIo(&io);
	/* Images always start at the beginning of the stream they are handed over
	 * in, regardless of what a previous component has read. */
	bool res = SUCCEEDED(stream->Seek(zero, STREAM_SEEK_SET, nullptr)) && FreeImage_ValidateFromHandle(fif, &io, stream) != FALSE;
	if ( ! res ) {
		/* Formats without a signature are recognized by the file name. This
		 * is only accepted if the content does not belong to another format. */
		STATSTG stat;
		memset(&stat, 0, sizeof(stat));
		if (SUCCEEDED(stream->Stat(&stat, STATFLAG_DEFAULT)) && stat.pwcsName != nullptr) {
			res = extensionMatches(fifExtensions(fif, L"", L';').c_str(), std::wstring(stat.pwcsName));
			CoTaskMemFree(stat.pwcsName);
		}
		if ( res ) {
			stream->Seek(zero, STREAM_SEEK_SET, nullptr);
			const FREE_IMAGE_FORMAT other = FreeImage_GetFileTypeFromHandle(&io, stream, 0);
			res = other == FIF_UNKNOWN || other == fif;
		}
	}
	stream->Seek(back, STREAM_SEEK_SET, nullptr);
	return res;
}


HRESULT createComponentInfo(REFCLSID clsid, REFIID riid, void ** ppv) {
	if (ppv == nullptr) {
		return E_POINTER;
	}
	*ppv = nullptr;
	ComPtr<IWICImagingFactory> factory;
	HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
	if ( FAILED(hr) ) {
		return hr;
	}
	ComPtr<IWICComponentInfo> info;
	hr = factory->CreateComponentInfo(clsid, info.GetAddressOf());
	if ( FAILED(hr) ) {
		return hr;
	}
	return info->QueryInterface(riid, ppv);
}
