/**
 * @file fiwicdec.cpp
 * @author Daniel Starke
 * @date 2024-12-03
 * @version 2026-10-26
 *
 * WIC decoder and frame decoder on top of FreeImage.
 *
 * The complete stream is read into memory on initialization. Single page
 * formats are decoded right away to report malformed files early. Pages of
 * multi page formats are decoded on first access.
 */
#include <string.h>
#include <windows.h>
#include <objbase.h>
#include <wincodec.h>
#include <vector>
#include <FreeImage.h>
#include <fiwicutil.hpp>


/**
 * Expands a floating point RGB bitmap by an opaque alpha channel.
 *
 * @param[in] src - `FIT_RGBF` bitmap
 * @return `FIT_RGBAF` bitmap or `nullptr`
 */
static FIBITMAP * convertRgbfToRgbaf(FIBITMAP * src) {
	const unsigned width = FreeImage_GetWidth(src);
	const unsigned height = FreeImage_GetHeight(src);
	FIBITMAP * dst = FreeImage_AllocateT(FIT_RGBAF, int(width), int(height));
	if (dst == nullptr) {
		return nullptr;
	}
	for (unsigned y = 0; y < height; ++y) {
		const FIRGBF * in = reinterpret_cast<const FIRGBF *>(FreeImage_GetScanLine(src, int(y)));
		FIRGBAF * out = reinterpret_cast<FIRGBAF *>(FreeImage_GetScanLine(dst, int(y)));
		for (unsigned x = 0; x < width; ++x) {
			out[x].red = in[x].red;
			out[x].green = in[x].green;
			out[x].blue = in[x].blue;
			out[x].alpha = 1.0f;
		}
	}
	return dst;
}


/**
 * Brings a decoded bitmap into a layout that WIC can consume directly.
 *
 * @param[in] src - decoded bitmap
 * @param[out] info - pixel format of the returned bitmap
 * @return prepared bitmap (may be `src` itself) or `nullptr` on failure
 */
static Dib prepareBitmap(const Dib & src, const PixelInfo ** info) {
	FIBITMAP * dib = src.get();
	FIBITMAP * conv = nullptr;
	switch (FreeImage_GetImageType(dib)) {
	case FIT_BITMAP: {
		const unsigned bpp = FreeImage_GetBPP(dib);
		if (bpp <= 8 && FreeImage_GetTransparencyCount(dib) > 0) {
			/* WIC has no palette alpha. */
			conv = FreeImage_ConvertTo32Bits(dib);
			*info = findPixelInfo(GUID_WICPixelFormat32bppBGRA);
			break;
		}
		switch (bpp) {
		case 1:
			*info = findPixelInfo(GUID_WICPixelFormat1bppIndexed);
			return src;
		case 4:
			*info = findPixelInfo(GUID_WICPixelFormat4bppIndexed);
			return src;
		case 8:
			*info = findPixelInfo((FreeImage_GetColorType(dib) == FIC_MINISBLACK) ? GUID_WICPixelFormat8bppGray : GUID_WICPixelFormat8bppIndexed);
			return src;
		case 24:
			*info = findPixelInfo(GUID_WICPixelFormat24bppBGR);
			return src;
		case 32:
			*info = findPixelInfo(GUID_WICPixelFormat32bppBGRA);
			return src;
		default:
			conv = FreeImage_ConvertTo24Bits(dib);
			*info = findPixelInfo(GUID_WICPixelFormat24bppBGR);
			break;
		}
		break;
	}
	case FIT_UINT16:
		*info = findPixelInfo(GUID_WICPixelFormat16bppGray);
		return src;
	case FIT_RGB16:
		*info = findPixelInfo(GUID_WICPixelFormat48bppRGB);
		return src;
	case FIT_RGBA16:
		*info = findPixelInfo(GUID_WICPixelFormat64bppRGBA);
		return src;
	case FIT_FLOAT:
		*info = findPixelInfo(GUID_WICPixelFormat32bppGrayFloat);
		return src;
	case FIT_RGBAF:
		*info = findPixelInfo(GUID_WICPixelFormat128bppRGBAFloat);
		return src;
	case FIT_RGBF:
		conv = convertRgbfToRgbaf(dib);
		*info = findPixelInfo(GUID_WICPixelFormat128bppRGBAFloat);
		break;
	default: {
		/* Signed and 32 bit integer, double and complex data. */
		FIBITMAP * std8 = FreeImage_ConvertToStandardType(dib, TRUE);
		if (std8 == nullptr) {
			return Dib();
		}
		const Dib owned = makeDib(std8);
		return prepareBitmap(owned, info);
	}
	}
	return makeDib(conv);
}


/**
 * One decoded image of a decoder.
 */
class FrameDecode : public ComObject<IWICBitmapFrameDecode> {
private:
	GUID container; /**< Container format of the image. */
	Dib source; /**< Bitmap as decoded by FreeImage (metadata, profile). */
	Dib dib; /**< Bitmap in the pixel format reported to WIC. */
	const PixelInfo * info; /**< Pixel format of `dib`. */
	UINT width; /**< Width in pixels. */
	UINT height; /**< Height in pixels. */
public:
	/**
	 * Creates a frame for a decoded bitmap.
	 *
	 * @param[in] container - container format of the image
	 * @param[in] source - bitmap as decoded by FreeImage
	 * @param[out] out - created frame
	 * @return result code
	 */
	static HRESULT create(const GUID & container, const Dib & source, IWICBitmapFrameDecode ** out) {
		*out = nullptr;
		try {
			const PixelInfo * info = nullptr;
			const Dib dib = prepareBitmap(source, &info);
			if (( ! dib ) || info == nullptr) {
				return WINCODEC_ERR_BADIMAGE;
			}
			FrameDecode * frame = new(std::nothrow) FrameDecode(container, source, dib, info);
			if (frame == nullptr) {
				return E_OUTOFMEMORY;
			}
			*out = frame;
			return S_OK;
		}
		FIWIC_CATCH_ALL
	}

	STDMETHOD(GetSize)(UINT * puiWidth, UINT * puiHeight) noexcept override {
		if (puiWidth == nullptr || puiHeight == nullptr) {
			return E_POINTER;
		}
		*puiWidth = this->width;
		*puiHeight = this->height;
		return S_OK;
	}

	STDMETHOD(GetPixelFormat)(WICPixelFormatGUID * pPixelFormat) noexcept override {
		if (pPixelFormat == nullptr) {
			return E_POINTER;
		}
		*pPixelFormat = *this->info->format;
		return S_OK;
	}

	STDMETHOD(GetResolution)(double * pDpiX, double * pDpiY) noexcept override {
		if (pDpiX == nullptr || pDpiY == nullptr) {
			return E_POINTER;
		}
		const unsigned dpmX = FreeImage_GetDotsPerMeterX(this->source.get());
		const unsigned dpmY = FreeImage_GetDotsPerMeterY(this->source.get());
		*pDpiX = (dpmX != 0) ? (double(dpmX) * 0.0254) : 96.0;
		*pDpiY = (dpmY != 0) ? (double(dpmY) * 0.0254) : 96.0;
		return S_OK;
	}

	STDMETHOD(CopyPalette)(IWICPalette * pIPalette) noexcept override {
		if (pIPalette == nullptr) {
			return E_POINTER;
		}
		if ( ! this->info->indexed ) {
			return WINCODEC_ERR_PALETTEUNAVAILABLE;
		}
		try {
			const RGBQUAD * palette = FreeImage_GetPalette(this->dib.get());
			const unsigned count = FreeImage_GetColorsUsed(this->dib.get());
			if (palette == nullptr || count == 0) {
				return WINCODEC_ERR_PALETTEUNAVAILABLE;
			}
			std::vector<WICColor> colors(count);
			for (unsigned i = 0; i < count; ++i) {
				colors[i] = 0xFF000000u
					| (WICColor(palette[i].rgbRed) << 16)
					| (WICColor(palette[i].rgbGreen) << 8)
					| WICColor(palette[i].rgbBlue);
			}
			return pIPalette->InitializeCustom(&colors[0], count);
		}
		FIWIC_CATCH_ALL
	}

	STDMETHOD(CopyPixels)(const WICRect * prc, UINT cbStride, UINT cbBufferSize, BYTE * pbBuffer) noexcept override {
		typedef unsigned long long u64;
		if (pbBuffer == nullptr) {
			return E_POINTER;
		}
		WICRect rc;
		if (prc != nullptr) {
			rc = *prc;
		} else {
			rc.X = 0;
			rc.Y = 0;
			rc.Width = INT(this->width);
			rc.Height = INT(this->height);
		}
		if (rc.X < 0 || rc.Y < 0 || rc.Width < 0 || rc.Height < 0
			|| (u64(rc.X) + u64(rc.Width)) > this->width
			|| (u64(rc.Y) + u64(rc.Height)) > this->height) {
			return E_INVALIDARG;
		}
		if (rc.Width == 0 || rc.Height == 0) {
			return S_OK;
		}
		const u64 bitOffset = u64(rc.X) * this->info->bpp;
		const unsigned shift = unsigned(bitOffset % 8);
		const u64 rowBits = u64(rc.Width) * this->info->bpp;
		const u64 rowBytes = (rowBits + 7) / 8;
		if (cbStride < rowBytes) {
			return E_INVALIDARG;
		}
		if (cbBufferSize < (u64(cbStride) * u64(rc.Height - 1) + rowBytes)) {
			return WINCODEC_ERR_INSUFFICIENTBUFFER;
		}
		/* FreeImage stores the bottom row first. */
		for (UINT row = 0; row < UINT(rc.Height); ++row) {
			const UINT srcRow = this->height - 1 - (UINT(rc.Y) + row);
			const BYTE * line = FreeImage_GetScanLine(this->dib.get(), int(srcRow));
			BYTE * dst = pbBuffer + size_t(row) * cbStride;
			const BYTE * src = line + (bitOffset / 8);
			if (shift == 0) {
				memcpy(dst, src, size_t(rowBytes));
				continue;
			}
			/* Sub byte pixels of a rectangle which does not start at a byte border. */
			const u64 avail = u64(FreeImage_GetLine(this->dib.get())) - (bitOffset / 8);
			for (u64 i = 0; i < rowBytes; ++i) {
				unsigned v = unsigned(src[i]) << shift;
				if ((i + 1) < avail) {
					v |= unsigned(src[i + 1]) >> (8 - shift);
				}
				dst[i] = BYTE(v & 0xFF);
			}
			if ((rowBits % 8) != 0) {
				dst[rowBytes - 1] = BYTE(dst[rowBytes - 1] & (0xFF << (8 - (rowBits % 8))));
			}
		}
		return S_OK;
	}

	STDMETHOD(GetMetadataQueryReader)(IWICMetadataQueryReader ** ppIMetadataQueryReader) noexcept override {
		if (ppIMetadataQueryReader == nullptr) {
			return E_POINTER;
		}
		return createMetadataQueryReader(this->source.get(), this->container, true, ppIMetadataQueryReader);
	}

	STDMETHOD(GetColorContexts)(UINT cCount, IWICColorContext ** ppIColorContexts, UINT * pcActualCount) noexcept override {
		if (pcActualCount == nullptr) {
			return E_POINTER;
		}
		FIICCPROFILE * icc = FreeImage_GetICCProfile(this->source.get());
		const bool hasProfile = icc != nullptr && icc->data != nullptr && icc->size > 0 && (icc->flags & FIICC_COLOR_IS_CMYK) == 0;
		*pcActualCount = hasProfile ? 1 : 0;
		if (( ! hasProfile ) || cCount == 0 || ppIColorContexts == nullptr || ppIColorContexts[0] == nullptr) {
			return S_OK;
		}
		return ppIColorContexts[0]->InitializeFromMemory(static_cast<const BYTE *>(icc->data), icc->size);
	}

	STDMETHOD(GetThumbnail)(IWICBitmapSource ** ppIThumbnail) noexcept override {
		if (ppIThumbnail == nullptr) {
			return E_POINTER;
		}
		*ppIThumbnail = nullptr;
		try {
			FIBITMAP * thumb = FreeImage_GetThumbnail(this->source.get());
			if (thumb == nullptr) {
				return WINCODEC_ERR_CODECNOTHUMBNAIL;
			}
			FIBITMAP * copy = FreeImage_Clone(thumb);
			if (copy == nullptr) {
				return E_OUTOFMEMORY;
			}
			ComPtr<IWICBitmapFrameDecode> frame;
			const HRESULT hr = FrameDecode::create(this->container, makeDib(copy), frame.GetAddressOf());
			if ( FAILED(hr) ) {
				return hr;
			}
			return frame->QueryInterface(__uuidof(IWICBitmapSource), reinterpret_cast<void **>(ppIThumbnail));
		}
		FIWIC_CATCH_ALL
	}
protected:
	bool provideInterface(REFIID riid, void ** ppv) noexcept override {
		if (riid == __uuidof(IWICBitmapSource)) {
			*ppv = static_cast<IWICBitmapSource *>(this);
			return true;
		}
		return ComObject<IWICBitmapFrameDecode>::provideInterface(riid, ppv);
	}
private:
	/**
	 * Constructor.
	 *
	 * @param[in] aContainer - container format of the image
	 * @param[in] aSource - bitmap as decoded by FreeImage
	 * @param[in] aDib - bitmap in the reported pixel format
	 * @param[in] aInfo - pixel format of `aDib`
	 */
	FrameDecode(const GUID & aContainer, const Dib & aSource, const Dib & aDib, const PixelInfo * aInfo):
		ComObject<IWICBitmapFrameDecode>(),
		container(aContainer),
		source(aSource),
		dib(aDib),
		info(aInfo),
		width(FreeImage_GetWidth(aDib.get())),
		height(FreeImage_GetHeight(aDib.get()))
	{}

	/** Destructor. */
	~FrameDecode() noexcept override = default;
};


/**
 * WIC decoder of one FreeImage format.
 */
class Decoder : public ComObject<IWICBitmapDecoder> {
private:
	FREE_IMAGE_FORMAT fif; /**< Decoded FreeImage format. */
	GUID container; /**< Container format reported to WIC. */
	CriticalSection lock; /**< Guards the members below. */
	ComPtr<IStream> stream; /**< Memory copy of the input; FreeImage reads from it. */
	FreeImageIO io; /**< FreeImage callbacks on `stream`. */
	FIMULTIBITMAP * multi; /**< Opened multi page bitmap (`nullptr` for single page). */
	std::vector<Dib> pages; /**< Decoded pages; empty entries are not decoded yet. */
	bool initialized; /**< `true` after a successful `Initialize`. */
public:
	/**
	 * Constructor.
	 *
	 * @param[in] aFif - FreeImage format to decode
	 */
	explicit Decoder(FREE_IMAGE_FORMAT aFif):
		ComObject<IWICBitmapDecoder>(),
		fif(aFif),
		container(formatGuid(FIWIC_GUID_CONTAINER, aFif)),
		lock(),
		stream(),
		io(),
		multi(nullptr),
		pages(),
		initialized(false)
	{
		initIo(&(this->io));
	}

	/** Destructor. */
	~Decoder() noexcept override {
		this->reset();
	}

	STDMETHOD(QueryCapability)(IStream * pIStream, DWORD * pdwCapability) noexcept override {
		if (pIStream == nullptr || pdwCapability == nullptr) {
			return E_POINTER;
		}
		*pdwCapability = 0;
		try {
			if ( ! validateStream(this->fif, pIStream) ) {
				return WINCODEC_ERR_UNKNOWNIMAGEFORMAT;
			}
		}
		FIWIC_CATCH_ALL
		*pdwCapability = WICBitmapDecoderCapabilityCanDecodeAllImages
			| WICBitmapDecoderCapabilityCanDecodeSomeImages
			| WICBitmapDecoderCapabilityCanEnumerateMetadata;
		return S_OK;
	}

	STDMETHOD(Initialize)(IStream * pIStream, WICDecodeOptions cacheOptions) noexcept override {
		if (pIStream == nullptr) {
			return E_POINTER;
		}
		Guard guard(this->lock);
		if ( this->initialized ) {
			return WINCODEC_ERR_WRONGSTATE;
		}
		this->reset();
		try {
			/* WIC offers the stream to decoders until one accepts it. The loaders
			 * of some formats accept arbitrary data, so check the content first. */
			if ( ! validateStream(this->fif, pIStream) ) {
				return WINCODEC_ERR_UNKNOWNIMAGEFORMAT;
			}
			HRESULT hr = streamAsMemory(pIStream, this->stream);
			if ( FAILED(hr) ) {
				return hr;
			}
			IStream * handle = this->stream.Get();
			if ( isMultiPageFormat(this->fif) ) {
				this->multi = FreeImage_OpenMultiBitmapFromHandle(this->fif, &(this->io), handle, 0);
			}
			if (this->multi != nullptr) {
				const int count = FreeImage_GetPageCount(this->multi);
				if (count <= 0) {
					this->reset();
					return WINCODEC_ERR_BADIMAGE;
				}
				this->pages.resize(size_t(count));
			} else {
				/* A failed multi page attempt has moved the stream position. */
				LARGE_INTEGER zero;
				zero.QuadPart = 0;
				this->stream->Seek(zero, STREAM_SEEK_SET, nullptr);
				FIBITMAP * dib = FreeImage_LoadFromHandle(this->fif, &(this->io), handle, 0);
				if (dib == nullptr) {
					this->reset();
					return WINCODEC_ERR_BADIMAGE;
				}
				this->pages.push_back(makeDib(dib));
			}
			this->initialized = true;
			return S_OK;
		}
		catch (const std::bad_alloc &) {
			this->reset();
			return E_OUTOFMEMORY;
		} catch (...) {
			this->reset();
			return E_FAIL;
		}
	}

	STDMETHOD(GetContainerFormat)(GUID * pguidContainerFormat) noexcept override {
		if (pguidContainerFormat == nullptr) {
			return E_POINTER;
		}
		*pguidContainerFormat = this->container;
		return S_OK;
	}

	STDMETHOD(GetDecoderInfo)(IWICBitmapDecoderInfo ** ppIDecoderInfo) noexcept override {
		if (ppIDecoderInfo == nullptr) {
			return E_POINTER;
		}
		return createComponentInfo(formatGuid(FIWIC_GUID_DECODER, this->fif), __uuidof(IWICBitmapDecoderInfo), reinterpret_cast<void **>(ppIDecoderInfo));
	}

	STDMETHOD(CopyPalette)(IWICPalette * pIPalette) noexcept override {
		return WINCODEC_ERR_PALETTEUNAVAILABLE;
	}

	STDMETHOD(GetMetadataQueryReader)(IWICMetadataQueryReader ** ppIMetadataQueryReader) noexcept override {
		/* All metadata is exposed at frame level. */
		return WINCODEC_ERR_UNSUPPORTEDOPERATION;
	}

	STDMETHOD(GetPreview)(IWICBitmapSource ** ppIBitmapSource) noexcept override {
		return WINCODEC_ERR_UNSUPPORTEDOPERATION;
	}

	STDMETHOD(GetColorContexts)(UINT cCount, IWICColorContext ** ppIColorContexts, UINT * pcActualCount) noexcept override {
		return WINCODEC_ERR_UNSUPPORTEDOPERATION;
	}

	STDMETHOD(GetThumbnail)(IWICBitmapSource ** ppIThumbnail) noexcept override {
		return WINCODEC_ERR_CODECNOTHUMBNAIL;
	}

	STDMETHOD(GetFrameCount)(UINT * pCount) noexcept override {
		if (pCount == nullptr) {
			return E_POINTER;
		}
		Guard guard(this->lock);
		if ( ! this->initialized ) {
			return WINCODEC_ERR_NOTINITIALIZED;
		}
		*pCount = UINT(this->pages.size());
		return S_OK;
	}

	STDMETHOD(GetFrame)(UINT index, IWICBitmapFrameDecode ** ppIBitmapFrame) noexcept override {
		if (ppIBitmapFrame == nullptr) {
			return E_POINTER;
		}
		*ppIBitmapFrame = nullptr;
		Guard guard(this->lock);
		if ( ! this->initialized ) {
			return WINCODEC_ERR_NOTINITIALIZED;
		}
		if (index >= this->pages.size()) {
			return WINCODEC_ERR_FRAMEMISSING;
		}
		try {
			if ( ! this->pages[index] ) {
				if (this->multi == nullptr) {
					return WINCODEC_ERR_BADIMAGE;
				}
				FIBITMAP * page = FreeImage_LockPage(this->multi, int(index));
				if (page == nullptr) {
					return WINCODEC_ERR_BADIMAGE;
				}
				/* The locked page belongs to the multi page bitmap. */
				FIBITMAP * copy = FreeImage_Clone(page);
				FreeImage_UnlockPage(this->multi, page, FALSE);
				if (copy == nullptr) {
					return E_OUTOFMEMORY;
				}
				this->pages[index] = makeDib(copy);
			}
			return FrameDecode::create(this->container, this->pages[index], ppIBitmapFrame);
		}
		FIWIC_CATCH_ALL
	}
private:
	/** Releases everything a (failed) `Initialize` has set up. */
	void reset() noexcept {
		if (this->multi != nullptr) {
			FreeImage_CloseMultiBitmap(this->multi, 0);
			this->multi = nullptr;
		}
		this->pages.clear();
		this->stream.Reset();
	}
};


HRESULT createDecoder(FREE_IMAGE_FORMAT fif, REFIID riid, void ** ppv) {
	if (ppv == nullptr) {
		return E_POINTER;
	}
	*ppv = nullptr;
	if ( ! isDecoderFormat(fif) ) {
		return CLASS_E_CLASSNOTAVAILABLE;
	}
	Decoder * decoder = new(std::nothrow) Decoder(fif);
	if (decoder == nullptr) {
		return E_OUTOFMEMORY;
	}
	const HRESULT hr = decoder->QueryInterface(riid, ppv);
	decoder->Release();
	return hr;
}
