/**
 * @file wictest.cpp
 * @author Daniel Starke
 * @date 2024-12-03
 * @version 2026-10-25
 *
 * Command-line test of the FreeImage WIC codec through the Windows Imaging
 * Component. Register fiwic.dll first (`regsvr32 fiwic.dll`).
 *
 *   wictest -list                    lists the registered FreeImage components
 *   wictest [-fiwic] [-meta] <file>  decodes all frames and prints their properties;
 *                                    -fiwic only considers the decoders of this codec,
 *                                    -meta prints the metadata items of every frame
 *   wictest <file> -encode <out>     additionally encodes the first frame to <out>
 *                                    (the format follows the extension) and decodes it again
 *   wictest -synth <type> -encode <out>
 *                                    encodes a generated 64x48 gradient instead of a file;
 *                                    type is gray16, grayf, rgb48, rgba16 or rgbaf
 */
#include <stdio.h>
#include <wchar.h>
#include <windows.h>
#include <objbase.h>
#include <wincodec.h>
/* Defines the GUIDs declared by fiwic.h in this program. */
#include <initguid.h>
#include <fiwic.h>

#include <string>
#include <vector>

#include <Utility.hpp>


using Microsoft::WRL::ComPtr;


/** `true` to print the metadata items of the decoded frames. */
static bool gPrintMetadata = false;


/** Initializes COM for the lifetime of the object. */
class ComScope {
private:
	HRESULT result; /**< Result of the initialization. */
public:
	/** Constructor. */
	ComScope() noexcept:
		result(CoInitializeEx(nullptr, COINIT_MULTITHREADED))
	{}

	/** Destructor. */
	~ComScope() noexcept {
		if ( SUCCEEDED(this->result) ) {
			CoUninitialize();
		}
	}

	/**
	 * Returns the result of the initialization.
	 *
	 * @return result code
	 */
	HRESULT initResult() const noexcept {
		return this->result;
	}
};


/** Pixel format with its display name. */
struct PixelFormatName {
	const GUID * format; /**< WIC pixel format. */
	const wchar_t * name; /**< Display name. */
};


/** Display names of the pixel formats the codec produces. */
static const PixelFormatName pixelFormatNames[] = {
	{&GUID_WICPixelFormat1bppIndexed, L"1bppIndexed"},
	{&GUID_WICPixelFormat4bppIndexed, L"4bppIndexed"},
	{&GUID_WICPixelFormat8bppIndexed, L"8bppIndexed"},
	{&GUID_WICPixelFormat8bppGray, L"8bppGray"},
	{&GUID_WICPixelFormat24bppBGR, L"24bppBGR"},
	{&GUID_WICPixelFormat32bppBGRA, L"32bppBGRA"},
	{&GUID_WICPixelFormat16bppGray, L"16bppGray"},
	{&GUID_WICPixelFormat48bppRGB, L"48bppRGB"},
	{&GUID_WICPixelFormat64bppRGBA, L"64bppRGBA"},
	{&GUID_WICPixelFormat32bppGrayFloat, L"32bppGrayFloat"},
	{&GUID_WICPixelFormat128bppRGBAFloat, L"128bppRGBAFloat"}
};


/**
 * Returns the display name of a pixel format.
 *
 * @param[in] format - WIC pixel format
 * @return display name
 */
static std::wstring pixelFormatText(const GUID & format) {
	for (size_t i = 0; i < (sizeof(pixelFormatNames) / sizeof(pixelFormatNames[0])); ++i) {
		if ( IsEqualGUID(*pixelFormatNames[i].format, format) ) {
			return pixelFormatNames[i].name;
		}
	}
	wchar_t buffer[40];
	return (StringFromGUID2(format, buffer, 40) != 0) ? std::wstring(buffer) : std::wstring(L"?");
}


/**
 * Returns the text form of a GUID.
 *
 * @param[in] guid - GUID
 * @return `{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}`
 */
static std::wstring guidText(const GUID & guid) {
	wchar_t buffer[40];
	return (StringFromGUID2(guid, buffer, 40) != 0) ? std::wstring(buffer) : std::wstring(L"?");
}


/**
 * Reads a string with the two call pattern used by the WIC info interfaces.
 *
 * @param[in] fn - callable `HRESULT (UINT, WCHAR *, UINT *)`
 * @return string or an empty string on failure
 * @tparam F - callable type
 */
template <typename F>
static std::wstring readText(F fn) {
	UINT length = 0;
	if (FAILED(fn(0, nullptr, &length)) || length == 0) {
		return std::wstring();
	}
	std::vector<wchar_t> buffer(length + 1, L'\0');
	UINT actual = 0;
	if ( FAILED(fn(length, &buffer[0], &actual)) ) {
		return std::wstring();
	}
	return std::wstring(&buffer[0]);
}


/**
 * Checks if a comma separated extension list contains an extension.
 *
 * @param[in] list - `.a,.b` style list
 * @param[in] ext - extension with leading dot
 * @return `true` on a match
 */
static bool extensionInList(const std::wstring & list, const std::wstring & ext) {
	size_t pos = 0;
	while (pos <= list.size()) {
		size_t end = list.find(L',', pos);
		if (end == std::wstring::npos) {
			end = list.size();
		}
		if (_wcsicmp(list.substr(pos, end - pos).c_str(), ext.c_str()) == 0) {
			return true;
		}
		pos = end + 1;
	}
	return false;
}


/**
 * Prints an error.
 *
 * @param[in] what - failed operation
 * @param[in] hr - result code
 * @return `false`
 */
static bool fail(const wchar_t * what, HRESULT hr) {
	fwprintf(stderr, L"error: %ls failed (0x%08lX)\n", what, static_cast<unsigned long>(hr));
	return false;
}


/**
 * Creates the WIC imaging factory.
 *
 * @param[out] factory - receiving factory
 * @return `true` on success
 */
static bool createFactory(ComPtr<IWICImagingFactory> & factory) {
	ComPtr<IWICImagingFactory> created;
	const HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&created));
	if ( FAILED(hr) ) {
		return fail(L"CoCreateInstance(CLSID_WICImagingFactory)", hr);
	}
	factory = created;
	return true;
}


/**
 * Lists the components of this codec known to WIC.
 *
 * @param[in] factory - WIC factory
 * @return number of found components
 */
static int listComponents(IWICImagingFactory * factory) {
	int found = 0;
	const WICComponentType types[] = {WICDecoder, WICEncoder};
	for (size_t t = 0; t < (sizeof(types) / sizeof(types[0])); ++t) {
		ComPtr<IEnumUnknown> list;
		/* WIC 1.0 (Windows XP, Vista) enumerates unsigned components only on request. */
		if ( FAILED(factory->CreateComponentEnumerator(types[t], WICComponentEnumerateUnsigned, list.GetAddressOf())) ) {
			continue;
		}
		for (;;) {
			ComPtr<IUnknown> item;
			ULONG fetched = 0;
			if (list->Next(1, item.GetAddressOf(), &fetched) != S_OK || fetched == 0) {
				break;
			}
			ComPtr<IWICBitmapCodecInfo> info;
			if ( FAILED(item->QueryInterface(__uuidof(IWICBitmapCodecInfo), reinterpret_cast<void **>(info.GetAddressOf()))) ) {
				continue;
			}
			GUID vendor;
			if (FAILED(info->GetVendorGUID(&vendor)) || ( ! IsEqualGUID(vendor, GUID_Vendor_FIWIC) )) {
				continue;
			}
			const std::wstring name = readText([&](UINT n, WCHAR * b, UINT * l) { return info->GetFriendlyName(n, b, l); });
			const std::wstring exts = readText([&](UINT n, WCHAR * b, UINT * l) { return info->GetFileExtensions(n, b, l); });
			wprintf(L"%ls  [%ls]\n", name.c_str(), exts.c_str());
			++found;
		}
	}
	wprintf(L"%d FreeImage WIC components registered\n", found);
	return found;
}


/**
 * Counts the metadata items WIC can enumerate for a frame and optionally prints them.
 *
 * @param[in] frame - decoded frame
 * @return item count
 */
static int countMetadata(IWICBitmapFrameDecode * frame) {
	ComPtr<IWICMetadataQueryReader> reader;
	if ( FAILED(frame->GetMetadataQueryReader(reader.GetAddressOf())) ) {
		return 0;
	}
	ComPtr<IEnumString> items;
	if ( FAILED(reader->GetEnumerator(items.GetAddressOf())) ) {
		return 0;
	}
	int count = 0;
	for (;;) {
		LPWSTR name = nullptr;
		ULONG fetched = 0;
		if (items->Next(1, &name, &fetched) != S_OK || fetched == 0) {
			break;
		}
		if ( gPrintMetadata ) {
			const std::wstring query = std::wstring(L"/") + name;
			PROPVARIANT value;
			PropVariantInit(&value);
			if (SUCCEEDED(reader->GetMetadataByName(query.c_str(), &value)) && value.vt == VT_LPWSTR) {
				wprintf(L"         %ls = %ls\n", name, value.pwszVal);
			} else {
				wprintf(L"         %ls (unreadable)\n", name);
			}
			PropVariantClear(&value);
		}
		CoTaskMemFree(name);
		++count;
	}
	return count;
}


/**
 * Computes a checksum over the pixels of an image.
 *
 * @param[in] source - image
 * @param[out] width - receiving width
 * @param[out] height - receiving height
 * @param[out] format - receiving pixel format
 * @param[out] checksum - receiving FNV-1a checksum of the rows without stride padding
 * @return `true` on success
 */
static bool checksum(IWICBitmapSource * source, UINT * width, UINT * height, WICPixelFormatGUID * format, unsigned long * checksum) {
	HRESULT hr = source->GetSize(width, height);
	if ( FAILED(hr) ) {
		return fail(L"GetSize", hr);
	}
	hr = source->GetPixelFormat(format);
	if ( FAILED(hr) ) {
		return fail(L"GetPixelFormat", hr);
	}
	UINT bitsPerPixel = 0;
	ComPtr<IWICImagingFactory> factory;
	ComPtr<IWICComponentInfo> componentInfo;
	ComPtr<IWICPixelFormatInfo> formatInfo;
	if (createFactory(factory)
		&& SUCCEEDED(factory->CreateComponentInfo(*format, componentInfo.GetAddressOf()))
		&& SUCCEEDED(componentInfo->QueryInterface(__uuidof(IWICPixelFormatInfo), reinterpret_cast<void **>(formatInfo.GetAddressOf())))) {
		formatInfo->GetBitsPerPixel(&bitsPerPixel);
	}
	if (bitsPerPixel == 0) {
		return fail(L"GetBitsPerPixel", WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT);
	}
	/* The stride is rounded up to a DWORD to also cover bit depths below 8. */
	const UINT stride = (((*width * bitsPerPixel) + 31) / 32) * 4;
	std::vector<BYTE> pixels(size_t(stride) * *height);
	hr = source->CopyPixels(nullptr, stride, UINT(pixels.size()), pixels.empty() ? nullptr : &pixels[0]);
	if ( FAILED(hr) ) {
		return fail(L"CopyPixels", hr);
	}
	unsigned long hash = 2166136261ul;
	const size_t rowBytes = (size_t(*width) * bitsPerPixel + 7) / 8;
	for (UINT y = 0; y < *height; ++y) {
		for (size_t x = 0; x < rowBytes; ++x) {
			hash = (hash ^ pixels[size_t(y) * stride + x]) * 16777619ul;
		}
	}
	*checksum = hash & 0xFFFFFFFFul;
	return true;
}


/**
 * Prints the properties of a frame and a checksum over its pixels.
 *
 * @param[in] index - frame index
 * @param[in] frame - decoded frame
 * @param[out] width - receiving width
 * @param[out] height - receiving height
 * @param[out] sum - receiving checksum of the pixel data
 * @return `true` on success
 */
static bool describeFrame(UINT index, IWICBitmapFrameDecode * frame, UINT * width, UINT * height, unsigned long * sum) {
	ComPtr<IWICBitmapSource> source;
	HRESULT hr = frame->QueryInterface(__uuidof(IWICBitmapSource), reinterpret_cast<void **>(source.GetAddressOf()));
	if ( FAILED(hr) ) {
		return fail(L"QueryInterface(IWICBitmapSource)", hr);
	}
	WICPixelFormatGUID format;
	if ( ! checksum(source.Get(), width, height, &format, sum) ) {
		return false;
	}
	double dpiX = 0.0;
	double dpiY = 0.0;
	frame->GetResolution(&dpiX, &dpiY);
	wprintf(L"frame %u: %ux%u, %ls, %.1fx%.1f dpi, %d metadata items, checksum %08lX\n",
		index, *width, *height, pixelFormatText(format).c_str(), dpiX, dpiY, countMetadata(frame), *sum);
	ComPtr<IWICImagingFactory> factory;
	ComPtr<IWICPalette> palette;
	UINT colors = 0;
	if (createFactory(factory) && SUCCEEDED(factory->CreatePalette(palette.GetAddressOf())) && SUCCEEDED(frame->CopyPalette(palette.Get()))
		&& SUCCEEDED(palette->GetColorCount(&colors))) {
		wprintf(L"         palette with %u colors\n", colors);
	}
	return true;
}


/**
 * Decodes a file and prints the properties of all frames.
 *
 * @param[in] factory - WIC factory
 * @param[in] path - image file
 * @param[in] onlyFiwic - `true` to only consider the decoders of this codec
 * @param[out] decoder - receiving decoder
 * @param[out] width - receiving width of the first frame
 * @param[out] height - receiving height of the first frame
 * @param[out] checksum - receiving checksum of the first frame
 * @return `true` on success
 */
static bool decodeFile(IWICImagingFactory * factory, const wchar_t * path, bool onlyFiwic, ComPtr<IWICBitmapDecoder> & decoder, UINT * width, UINT * height, unsigned long * checksum) {
	HRESULT hr = factory->CreateDecoderFromFilename(path, onlyFiwic ? &GUID_Vendor_FIWIC : nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, decoder.GetAddressOf());
	if ( FAILED(hr) ) {
		return fail(L"CreateDecoderFromFilename", hr);
	}
	ComPtr<IWICBitmapDecoderInfo> info;
	if ( SUCCEEDED(decoder->GetDecoderInfo(info.GetAddressOf())) ) {
		const std::wstring name = readText([&](UINT n, WCHAR * b, UINT * l) { return info->GetFriendlyName(n, b, l); });
		GUID container;
		decoder->GetContainerFormat(&container);
		wprintf(L"decoder: %ls %ls\n", name.c_str(), guidText(container).c_str());
	}
	/* Windows asks every candidate decoder for its capability before using it. */
	{
		GUID container;
		ComPtr<IWICBitmapDecoder> fresh;
		ComPtr<IWICStream> stream;
		if (SUCCEEDED(decoder->GetContainerFormat(&container))
			&& SUCCEEDED(factory->CreateDecoder(container, nullptr, fresh.GetAddressOf()))
			&& SUCCEEDED(factory->CreateStream(stream.GetAddressOf()))
			&& SUCCEEDED(stream->InitializeFromFilename(path, GENERIC_READ))) {
			DWORD capability = 0;
			hr = fresh->QueryCapability(stream.Get(), &capability);
			wprintf(L"capability: 0x%02lX (0x%08lX)\n", static_cast<unsigned long>(capability), static_cast<unsigned long>(hr));
		}
	}
	UINT count = 0;
	hr = decoder->GetFrameCount(&count);
	if ( FAILED(hr) ) {
		return fail(L"GetFrameCount", hr);
	}
	wprintf(L"frames: %u\n", count);
	for (UINT i = 0; i < count; ++i) {
		ComPtr<IWICBitmapFrameDecode> frame;
		hr = decoder->GetFrame(i, frame.GetAddressOf());
		if ( FAILED(hr) ) {
			return fail(L"GetFrame", hr);
		}
		UINT w = 0;
		UINT h = 0;
		unsigned long sum = 0;
		if ( ! describeFrame(i, frame.Get(), &w, &h, &sum) ) {
			return false;
		}
		if (i == 0) {
			*width = w;
			*height = h;
			*checksum = sum;
		}
	}
	return true;
}


/**
 * Encodes an image into a file.
 * The encoder is selected by the extension of the output file.
 *
 * @param[in] factory - WIC factory
 * @param[in] source - image to encode
 * @param[in] path - output file
 * @return `true` on success
 */
static bool encodeFile(IWICImagingFactory * factory, IWICBitmapSource * source, const wchar_t * path) {
	const wchar_t * dot = wcsrchr(path, L'.');
	if (dot == nullptr) {
		fwprintf(stderr, L"error: output file needs an extension\n");
		return false;
	}
	/* Find the encoder registered for the extension. */
	ComPtr<IEnumUnknown> list;
	HRESULT hr = factory->CreateComponentEnumerator(WICEncoder, WICComponentEnumerateUnsigned, list.GetAddressOf());
	if ( FAILED(hr) ) {
		return fail(L"CreateComponentEnumerator", hr);
	}
	GUID container = GUID_NULL;
	for (;;) {
		ComPtr<IUnknown> item;
		ULONG fetched = 0;
		if (list->Next(1, item.GetAddressOf(), &fetched) != S_OK || fetched == 0) {
			break;
		}
		ComPtr<IWICBitmapEncoderInfo> info;
		if ( FAILED(item->QueryInterface(__uuidof(IWICBitmapEncoderInfo), reinterpret_cast<void **>(info.GetAddressOf()))) ) {
			continue;
		}
		const std::wstring exts = readText([&](UINT n, WCHAR * b, UINT * l) { return info->GetFileExtensions(n, b, l); });
		if ( extensionInList(exts, dot) ) {
			info->GetContainerFormat(&container);
			break;
		}
	}
	if ( IsEqualGUID(container, GUID_NULL) ) {
		fwprintf(stderr, L"error: no encoder registered for %ls\n", dot);
		return false;
	}
	ComPtr<IWICBitmapEncoder> encoder;
	hr = factory->CreateEncoder(container, nullptr, encoder.GetAddressOf());
	if ( FAILED(hr) ) {
		return fail(L"CreateEncoder", hr);
	}
	ComPtr<IWICStream> stream;
	hr = factory->CreateStream(stream.GetAddressOf());
	if ( SUCCEEDED(hr) ) {
		hr = stream->InitializeFromFilename(path, GENERIC_WRITE);
	}
	if ( FAILED(hr) ) {
		return fail(L"InitializeFromFilename", hr);
	}
	hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
	if ( FAILED(hr) ) {
		return fail(L"encoder Initialize", hr);
	}
	UINT width = 0;
	UINT height = 0;
	source->GetSize(&width, &height);
	WICPixelFormatGUID format;
	source->GetPixelFormat(&format);
	double dpiX = 96.0;
	double dpiY = 96.0;
	source->GetResolution(&dpiX, &dpiY);
	ComPtr<IWICBitmapFrameEncode> frame;
	ComPtr<IPropertyBag2> options;
	hr = encoder->CreateNewFrame(frame.GetAddressOf(), options.GetAddressOf());
	if ( FAILED(hr) ) {
		return fail(L"CreateNewFrame", hr);
	}
	hr = frame->Initialize(options.Get());
	if ( SUCCEEDED(hr) ) {
		hr = frame->SetSize(width, height);
	}
	if ( SUCCEEDED(hr) ) {
		hr = frame->SetResolution(dpiX, dpiY);
	}
	if ( FAILED(hr) ) {
		return fail(L"frame setup", hr);
	}
	WICPixelFormatGUID target = format;
	hr = frame->SetPixelFormat(&target);
	if ( FAILED(hr) ) {
		return fail(L"SetPixelFormat", hr);
	}
	wprintf(L"encoding as %ls (source %ls)\n", pixelFormatText(target).c_str(), pixelFormatText(format).c_str());
	hr = frame->WriteSource(source, nullptr);
	if ( SUCCEEDED(hr) ) {
		hr = frame->Commit();
	}
	if ( FAILED(hr) ) {
		return fail(L"WriteSource/Commit", hr);
	}
	hr = encoder->Commit();
	if ( FAILED(hr) ) {
		return fail(L"encoder Commit", hr);
	}
	stream->Commit(STGC_DEFAULT);
	return true;
}


/**
 * Writes a floating point number into a pixel buffer.
 *
 * @param[out] dst - destination
 * @param[in] value - number
 */
static void writeFloat(BYTE * dst, float value) {
	memcpy(dst, &value, sizeof(value));
}


/**
 * Writes a 16 bit number into a pixel buffer.
 *
 * @param[out] dst - destination
 * @param[in] value - number
 */
static void writeWord(BYTE * dst, unsigned value) {
	dst[0] = BYTE(value & 0xFF);
	dst[1] = BYTE((value >> 8) & 0xFF);
}


/**
 * Creates a 64x48 gradient image in a pixel format which no 8 bit source converts to on every WIC implementation.
 *
 * @param[in] factory - WIC factory
 * @param[in] type - `gray16`, `grayf`, `rgb48`, `rgba16` or `rgbaf`
 * @param[out] out - generated image
 * @return `true` on success
 */
static bool createSynthetic(IWICImagingFactory * factory, const wchar_t * type, ComPtr<IWICBitmapSource> & out) {
	const GUID * format = nullptr;
	unsigned channels = 0;
	bool isFloat = false;
	if (wcscmp(type, L"gray16") == 0) {
		format = &GUID_WICPixelFormat16bppGray;
		channels = 1;
	} else if (wcscmp(type, L"grayf") == 0) {
		format = &GUID_WICPixelFormat32bppGrayFloat;
		channels = 1;
		isFloat = true;
	} else if (wcscmp(type, L"rgb48") == 0) {
		format = &GUID_WICPixelFormat48bppRGB;
		channels = 3;
	} else if (wcscmp(type, L"rgba16") == 0) {
		format = &GUID_WICPixelFormat64bppRGBA;
		channels = 4;
	} else if (wcscmp(type, L"rgbaf") == 0) {
		format = &GUID_WICPixelFormat128bppRGBAFloat;
		channels = 4;
		isFloat = true;
	} else {
		fwprintf(stderr, L"error: unknown synthetic type %ls\n", type);
		return false;
	}
	const UINT width = 64;
	const UINT height = 48;
	const unsigned sampleBytes = isFloat ? 4 : 2;
	const UINT stride = width * channels * sampleBytes;
	/* The pixels are written to an own buffer because `IWICBitmapLock::GetDataPointer`
	 * fails with `E_NOTIMPL` in a multithreaded apartment on WIC 1.0 (Windows XP). */
	std::vector<BYTE> data(size_t(stride) * height);
	for (UINT y = 0; y < height; ++y) {
		for (UINT x = 0; x < width; ++x) {
			const double u = double(x) / (width - 1);
			const double v = double(y) / (height - 1);
			/* Floating point samples exceed 1.0 to cover high dynamic range data. */
			const double samples[4] = {u * (isFloat ? 4.0 : 1.0), v, (u + v) / 2.0, 1.0};
			BYTE * pixel = &data[0] + size_t(y) * stride + size_t(x) * channels * sampleBytes;
			for (unsigned c = 0; c < channels; ++c) {
				if ( isFloat ) {
					writeFloat(pixel + c * 4, float(samples[c]));
				} else {
					writeWord(pixel + c * 2, unsigned(samples[c] * 65535.0 + 0.5));
				}
			}
		}
	}
	ComPtr<IWICBitmap> bitmap;
	const HRESULT hr = factory->CreateBitmapFromMemory(width, height, *format, stride, UINT(data.size()), &data[0], bitmap.GetAddressOf());
	if ( FAILED(hr) ) {
		return fail(L"CreateBitmapFromMemory", hr);
	}
	return SUCCEEDED(bitmap->QueryInterface(__uuidof(IWICBitmapSource), reinterpret_cast<void **>(out.GetAddressOf())));
}


/**
 * Program entry.
 *
 * @param[in] argc - number of arguments
 * @param[in] argv - arguments
 * @return `0` on success
 */
int wmain(int argc, wchar_t ** argv) {
	const wchar_t * input = nullptr;
	const wchar_t * output = nullptr;
	const wchar_t * synthetic = nullptr;
	bool list = false;
	bool onlyFiwic = false;
	for (int i = 1; i < argc; ++i) {
		if (wcscmp(argv[i], L"-meta") == 0) {
			gPrintMetadata = true;
			continue;
		}
		if (wcscmp(argv[i], L"-list") == 0) {
			list = true;
		} else if (wcscmp(argv[i], L"-fiwic") == 0) {
			onlyFiwic = true;
		} else if (wcscmp(argv[i], L"-encode") == 0 && (i + 1) < argc) {
			output = argv[++i];
		} else if (wcscmp(argv[i], L"-synth") == 0 && (i + 1) < argc) {
			synthetic = argv[++i];
		} else if (input == nullptr && argv[i][0] != L'-') {
			input = argv[i];
		} else {
			input = nullptr;
			synthetic = nullptr;
			list = false;
			break;
		}
	}
	if (( ! list ) && input == nullptr && (synthetic == nullptr || output == nullptr)) {
		fwprintf(stderr,
			L"usage: wictest -list\n"
			 "       wictest [-fiwic] [-meta] <file> [-encode <out>]\n"
			 "       wictest -synth <gray16|grayf|rgb48|rgba16|rgbaf> -encode <out>\n"
		);
		return 2;
	}
	ComScope com;
	if ( FAILED(com.initResult()) ) {
		return fail(L"CoInitializeEx", com.initResult()) ? 0 : 1;
	}
	ComPtr<IWICImagingFactory> factory;
	if ( ! createFactory(factory) ) {
		return 1;
	}
	if ( list ) {
		return (listComponents(factory.Get()) > 0) ? 0 : 1;
	}
	ComPtr<IWICBitmapDecoder> decoder;
	ComPtr<IWICBitmapSource> source;
	UINT width = 0;
	UINT height = 0;
	unsigned long outSum = 0;
	if (synthetic != nullptr) {
		WICPixelFormatGUID format;
		if (( ! createSynthetic(factory.Get(), synthetic, source) ) || ( ! checksum(source.Get(), &width, &height, &format, &outSum) )) {
			return 1;
		}
		wprintf(L"synthetic %ls: %ux%u, %ls, checksum %08lX\n", synthetic, width, height, pixelFormatText(format).c_str(), outSum);
	} else {
		if ( ! decodeFile(factory.Get(), input, onlyFiwic, decoder, &width, &height, &outSum) ) {
			return 1;
		}
		ComPtr<IWICBitmapFrameDecode> frame;
		if (output != nullptr && (FAILED(decoder->GetFrame(0, frame.GetAddressOf()))
			|| FAILED(frame->QueryInterface(__uuidof(IWICBitmapSource), reinterpret_cast<void **>(source.GetAddressOf()))))) {
			return fail(L"GetFrame", E_FAIL) ? 0 : 1;
		}
	}
	if (output != nullptr) {
		if ( ! encodeFile(factory.Get(), source.Get(), output) ) {
			return 1;
		}
		ComPtr<IWICBitmapDecoder> check;
		UINT checkWidth = 0;
		UINT checkHeight = 0;
		unsigned long inSum = 0;
		wprintf(L"-- decoding %ls\n", output);
		if ( ! decodeFile(factory.Get(), output, onlyFiwic, check, &checkWidth, &checkHeight, &inSum) ) {
			return 1;
		}
		if (checkWidth != width || checkHeight != height) {
			fwprintf(stderr, L"error: size changed from %ux%u to %ux%u\n", width, height, checkWidth, checkHeight);
			return 1;
		}
		wprintf(L"round trip %ls\n", (inSum == outSum) ? L"lossless" : L"size preserved, pixels differ (conversion or lossy format)");
	}
	return 0;
}
