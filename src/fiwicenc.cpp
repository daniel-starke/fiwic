/**
 * @file fiwicenc.cpp
 * @author Daniel Starke
 * @date 2024-12-03
 * @version 2026-10-26
 *
 * WIC encoder and frame encoder on top of FreeImage.
 *
 * The supported FreeImage formats store a single image. The frame encoder
 * collects the pixels in a FreeImage bitmap which the encoder writes to its
 * stream on `Commit`.
 */
#include <string.h>
#include <windows.h>
#include <objbase.h>
#include <wincodec.h>
#include <vector>
#include <FreeImage.h>
#include <fiwicutil.hpp>


/**
 * Empty encoder option bag. The codec has no encoder options but WIC callers
 * expect an object when they ask for the options of a new frame.
 */
class PropertyBag : public ComObject<IPropertyBag2> {
public:
	/** Constructor. */
	PropertyBag():
		ComObject<IPropertyBag2>()
	{}

	/** Destructor. */
	~PropertyBag() noexcept override = default;

	STDMETHOD(Read)(ULONG cProperties, PROPBAG2 * pPropBag, IErrorLog * pErrLog, VARIANT * pvarValue, HRESULT * phrError) noexcept override {
		if (cProperties == 0) {
			return S_OK;
		}
		if (pPropBag == nullptr || pvarValue == nullptr) {
			return E_POINTER;
		}
		for (ULONG i = 0; i < cProperties && phrError != nullptr; ++i) {
			phrError[i] = E_INVALIDARG;
		}
		return E_INVALIDARG;
	}

	STDMETHOD(Write)(ULONG cProperties, PROPBAG2 * pPropBag, VARIANT * pvarValue) noexcept override {
		return (cProperties == 0) ? S_OK : E_INVALIDARG;
	}

	STDMETHOD(CountProperties)(ULONG * pcProperties) noexcept override {
		if (pcProperties == nullptr) {
			return E_POINTER;
		}
		*pcProperties = 0;
		return S_OK;
	}

	STDMETHOD(GetPropertyInfo)(ULONG iProperty, ULONG cProperties, PROPBAG2 * pPropBag, ULONG * pcProperties) noexcept override {
		if (pcProperties == nullptr) {
			return E_POINTER;
		}
		*pcProperties = 0;
		return (iProperty == 0 && cProperties == 0) ? S_OK : E_INVALIDARG;
	}

	STDMETHOD(LoadObject)(LPCOLESTR pstrName, DWORD dwHint, IUnknown * pUnkObject, IErrorLog * pErrLog) noexcept override {
		return E_NOTIMPL;
	}
};


/**
 * Reads the colors of a WIC palette.
 *
 * @param[in] palette - palette to read
 * @param[out] colors - receiving colors
 * @return result code
 */
static HRESULT readPalette(IWICPalette * palette, std::vector<WICColor> * colors) {
	UINT count = 0;
	HRESULT hr = palette->GetColorCount(&count);
	if ( FAILED(hr) ) {
		return hr;
	}
	colors->assign(count, 0);
	if (count == 0) {
		return S_OK;
	}
	UINT actual = 0;
	hr = palette->GetColors(count, &(*colors)[0], &actual);
	if ( SUCCEEDED(hr) ) {
		colors->resize(actual);
	}
	return hr;
}


/**
 * Copies palette colors into a FreeImage bitmap.
 *
 * @param[in] dib - palette bitmap
 * @param[in] colors - colors to set
 */
static void applyPalette(FIBITMAP * dib, const std::vector<WICColor> & colors) noexcept {
	RGBQUAD * palette = FreeImage_GetPalette(dib);
	if (palette == nullptr) {
		return;
	}
	const unsigned count = FreeImage_GetColorsUsed(dib);
	for (unsigned i = 0; i < count && i < colors.size(); ++i) {
		palette[i].rgbRed = BYTE((colors[i] >> 16) & 0xFF);
		palette[i].rgbGreen = BYTE((colors[i] >> 8) & 0xFF);
		palette[i].rgbBlue = BYTE(colors[i] & 0xFF);
		palette[i].rgbReserved = 0;
	}
}


/**
 * Removes the alpha channel of a floating point bitmap.
 *
 * @param[in] src - `FIT_RGBAF` bitmap
 * @return `FIT_RGBF` bitmap or `nullptr`
 */
static FIBITMAP * convertRgbafToRgbf(FIBITMAP * src) {
	const unsigned width = FreeImage_GetWidth(src);
	const unsigned height = FreeImage_GetHeight(src);
	FIBITMAP * dst = FreeImage_AllocateT(FIT_RGBF, int(width), int(height));
	if (dst == nullptr) {
		return nullptr;
	}
	for (unsigned y = 0; y < height; ++y) {
		const FIRGBAF * in = reinterpret_cast<const FIRGBAF *>(FreeImage_GetScanLine(src, int(y)));
		FIRGBF * out = reinterpret_cast<FIRGBF *>(FreeImage_GetScanLine(dst, int(y)));
		for (unsigned x = 0; x < width; ++x) {
			out[x].red = in[x].red;
			out[x].green = in[x].green;
			out[x].blue = in[x].blue;
		}
	}
	FreeImage_SetDotsPerMeterX(dst, FreeImage_GetDotsPerMeterX(src));
	FreeImage_SetDotsPerMeterY(dst, FreeImage_GetDotsPerMeterY(src));
	return dst;
}


/**
 * Converts a resolution to dots per meter.
 *
 * @param[in] dpi - dots per inch
 * @return dots per meter, clamped to the range FreeImage can store
 */
static unsigned dpiToDpm(double dpi) noexcept {
	double dpm = dpi / 0.0254 + 0.5;
	if ( ! (dpm >= 1.0) ) {
		dpm = 1.0;
	} else if (dpm > 4.0e9) {
		dpm = 4.0e9;
	}
	return unsigned(dpm);
}


/**
 * The single frame of an encoder.
 */
class FrameEncode : public ComObject<IWICBitmapFrameEncode> {
private:
	FREE_IMAGE_FORMAT fif; /**< Encoded FreeImage format. */
	const PixelInfo * info; /**< Pixel format expected by `WritePixels`. */
	bool initialized; /**< `true` after `Initialize`. */
	bool sizeSet; /**< `true` after `SetSize`. */
	bool committed; /**< `true` after `Commit`. */
	UINT width; /**< Width in pixels. */
	UINT height; /**< Height in pixels. */
	double dpiX; /**< Horizontal resolution. */
	double dpiY; /**< Vertical resolution. */
	std::vector<WICColor> palette; /**< Colors for indexed pixel formats. */
	Dib dib; /**< Collected image (allocated with the first pixels). */
	UINT lines; /**< Number of rows written so far. */
public:
	/**
	 * Constructor.
	 *
	 * @param[in] aFif - FreeImage format to encode
	 */
	explicit FrameEncode(FREE_IMAGE_FORMAT aFif):
		ComObject<IWICBitmapFrameEncode>(),
		fif(aFif),
		info(negotiatePixelFormat(aFif, GUID_WICPixelFormat32bppBGRA)),
		initialized(false),
		sizeSet(false),
		committed(false),
		width(0),
		height(0),
		dpiX(96.0),
		dpiY(96.0),
		palette(),
		dib(),
		lines(0)
	{}

	/** Destructor. */
	~FrameEncode() noexcept override = default;

	/**
	 * Returns the committed image.
	 *
	 * @return image or an empty pointer if the frame is not committed
	 */
	Dib bitmap() const {
		return this->committed ? this->dib : Dib();
	}

	STDMETHOD(Initialize)(IPropertyBag2 * pIEncoderOptions) noexcept override {
		if ( this->initialized ) {
			return WINCODEC_ERR_WRONGSTATE;
		}
		this->initialized = true;
		return S_OK;
	}

	STDMETHOD(SetSize)(UINT uiWidth, UINT uiHeight) noexcept override {
		if (( ! this->initialized ) || this->dib) {
			return WINCODEC_ERR_WRONGSTATE;
		}
		if (uiWidth == 0 || uiHeight == 0 || uiWidth > 0x7FFFFFFFu || uiHeight > 0x7FFFFFFFu) {
			return E_INVALIDARG;
		}
		this->width = uiWidth;
		this->height = uiHeight;
		this->sizeSet = true;
		return S_OK;
	}

	STDMETHOD(SetResolution)(double aDpiX, double aDpiY) noexcept override {
		if (( ! this->initialized ) || this->dib) {
			return WINCODEC_ERR_WRONGSTATE;
		}
		if (aDpiX < 0.0 || aDpiY < 0.0) {
			return E_INVALIDARG;
		}
		/* A resolution of zero means unspecified. */
		this->dpiX = (aDpiX > 0.0) ? aDpiX : 96.0;
		this->dpiY = (aDpiY > 0.0) ? aDpiY : 96.0;
		return S_OK;
	}

	STDMETHOD(SetPixelFormat)(WICPixelFormatGUID * pPixelFormat) noexcept override {
		if (pPixelFormat == nullptr) {
			return E_POINTER;
		}
		if (( ! this->initialized ) || this->dib) {
			return WINCODEC_ERR_WRONGSTATE;
		}
		const PixelInfo * negotiated = negotiatePixelFormat(this->fif, *pPixelFormat);
		if (negotiated == nullptr) {
			return WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
		}
		this->info = negotiated;
		*pPixelFormat = *negotiated->format;
		return S_OK;
	}

	STDMETHOD(SetColorContexts)(UINT cCount, IWICColorContext ** ppIColorContext) noexcept override {
		return WINCODEC_ERR_UNSUPPORTEDOPERATION;
	}

	STDMETHOD(SetPalette)(IWICPalette * pIPalette) noexcept override {
		if (pIPalette == nullptr) {
			return E_POINTER;
		}
		if ( ! this->initialized ) {
			return WINCODEC_ERR_NOTINITIALIZED;
		}
		try {
			const HRESULT hr = readPalette(pIPalette, &(this->palette));
			if (SUCCEEDED(hr) && this->dib && this->info != nullptr && this->info->indexed) {
				applyPalette(this->dib.get(), this->palette);
			}
			return hr;
		}
		FIWIC_CATCH_ALL
	}

	STDMETHOD(SetThumbnail)(IWICBitmapSource * pIThumbnail) noexcept override {
		return WINCODEC_ERR_UNSUPPORTEDOPERATION;
	}

	STDMETHOD(WritePixels)(UINT lineCount, UINT cbStride, UINT cbBufferSize, BYTE * pbPixels) noexcept override {
		typedef unsigned long long u64;
		if (pbPixels == nullptr) {
			return E_POINTER;
		}
		if (( ! this->initialized ) || ( ! this->sizeSet )) {
			return WINCODEC_ERR_NOTINITIALIZED;
		}
		if (this->committed || lineCount > (this->height - this->lines)) {
			return E_INVALIDARG;
		}
		if (lineCount == 0) {
			return S_OK;
		}
		try {
			const HRESULT hr = this->ensureBitmap();
			if ( FAILED(hr) ) {
				return hr;
			}
			const u64 rowBytes = (u64(this->width) * this->info->bpp + 7) / 8;
			if (cbStride < rowBytes) {
				return E_INVALIDARG;
			}
			if (cbBufferSize < (u64(cbStride) * (lineCount - 1) + rowBytes)) {
				return WINCODEC_ERR_INSUFFICIENTBUFFER;
			}
			/* WIC delivers the top row first, FreeImage stores the bottom row first. */
			for (UINT row = 0; row < lineCount; ++row) {
				BYTE * line = FreeImage_GetScanLine(this->dib.get(), int(this->height - 1 - (this->lines + row)));
				memcpy(line, pbPixels + size_t(row) * cbStride, size_t(rowBytes));
			}
			this->lines += lineCount;
			return S_OK;
		}
		FIWIC_CATCH_ALL
	}

	STDMETHOD(WriteSource)(IWICBitmapSource * pIBitmapSource, WICRect * prc) noexcept override {
		if (pIBitmapSource == nullptr) {
			return E_POINTER;
		}
		if ( ! this->initialized ) {
			return WINCODEC_ERR_NOTINITIALIZED;
		}
		try {
			UINT srcWidth = 0;
			UINT srcHeight = 0;
			HRESULT hr = pIBitmapSource->GetSize(&srcWidth, &srcHeight);
			if ( FAILED(hr) ) {
				return hr;
			}
			WICRect rc;
			if (prc != nullptr) {
				rc = *prc;
			} else {
				rc.X = 0;
				rc.Y = 0;
				rc.Width = INT(srcWidth);
				rc.Height = INT(srcHeight);
			}
			if (rc.Width <= 0 || rc.Height <= 0 || rc.X < 0 || rc.Y < 0
				|| (static_cast<long long>(rc.Y) + rc.Height) > 0x7FFFFFFFll) {
				return E_INVALIDARG;
			}
			if ( ! this->sizeSet ) {
				hr = this->SetSize(UINT(rc.Width), UINT(rc.Height));
				if ( FAILED(hr) ) {
					return hr;
				}
			} else if (UINT(rc.Width) != this->width || UINT(rc.Height) != (this->height - this->lines)) {
				return E_INVALIDARG;
			}
			if (this->info == nullptr) {
				return WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
			}
			/* Bring the source into the pixel format of the encoder. */
			ComPtr<IWICBitmapSource> source(pIBitmapSource);
			WICPixelFormatGUID format;
			hr = pIBitmapSource->GetPixelFormat(&format);
			if ( FAILED(hr) ) {
				return hr;
			}
			const bool convert = ( ! IsEqualGUID(format, *this->info->format) );
			if (convert || (this->info->indexed && this->palette.empty())) {
				hr = this->prepareSource(pIBitmapSource, convert, source);
				if ( FAILED(hr) ) {
					return hr;
				}
			}
			/* Copy the rows in chunks of about one megabyte. */
			/* 64 bit math: the product wraps on 32 bit targets. */
			const unsigned long long rowBytes64 = (static_cast<unsigned long long>(rc.Width) * this->info->bpp + 7) / 8;
			if (rowBytes64 > (1ull << 28)) {
				return E_OUTOFMEMORY;
			}
			const size_t rowBytes = size_t(rowBytes64);
			const UINT chunkRows = UINT(((1u << 20) / rowBytes) + 1);
			std::vector<BYTE> buffer(rowBytes * chunkRows);
			UINT done = 0;
			while (done < UINT(rc.Height)) {
				WICRect part;
				part.X = rc.X;
				part.Y = rc.Y + INT(done);
				part.Width = rc.Width;
				const UINT rows = (UINT(rc.Height) - done < chunkRows) ? (UINT(rc.Height) - done) : chunkRows;
				part.Height = INT(rows);
				hr = source->CopyPixels(&part, UINT(rowBytes), UINT(rowBytes * rows), &buffer[0]);
				if ( FAILED(hr) ) {
					return hr;
				}
				hr = this->WritePixels(rows, UINT(rowBytes), UINT(rowBytes * rows), &buffer[0]);
				if ( FAILED(hr) ) {
					return hr;
				}
				done += rows;
			}
			return S_OK;
		}
		FIWIC_CATCH_ALL
	}

	STDMETHOD(Commit)() noexcept override {
		if (( ! this->initialized ) || ( ! this->sizeSet )) {
			return WINCODEC_ERR_NOTINITIALIZED;
		}
		if (this->committed || ( ! this->dib ) || this->lines != this->height) {
			return WINCODEC_ERR_WRONGSTATE;
		}
		try {
			if (this->info->type == FIT_RGBAF && ( ! FreeImage_FIFSupportsExportType(this->fif, FIT_RGBAF) )) {
				/* Selected by the pixel format negotiation for RGB only formats. */
				FIBITMAP * rgb = convertRgbafToRgbf(this->dib.get());
				if (rgb == nullptr) {
					return E_OUTOFMEMORY;
				}
				this->dib = makeDib(rgb);
			}
			this->committed = true;
			return S_OK;
		}
		FIWIC_CATCH_ALL
	}

	STDMETHOD(GetMetadataQueryWriter)(IWICMetadataQueryWriter ** ppIMetadataQueryWriter) noexcept override {
		return WINCODEC_ERR_UNSUPPORTEDOPERATION;
	}
private:
	/**
	 * Allocates the FreeImage bitmap if not done yet.
	 *
	 * @return result code
	 */
	HRESULT ensureBitmap() {
		if ( this->dib ) {
			return S_OK;
		}
		if (this->info == nullptr) {
			return WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
		}
		FIBITMAP * newDib = FreeImage_AllocateT(this->info->type, int(this->width), int(this->height), int((this->info->type == FIT_BITMAP) ? this->info->bpp : 0));
		if (newDib == nullptr) {
			return E_OUTOFMEMORY;
		}
		this->dib = makeDib(newDib);
		if ( this->info->indexed ) {
			applyPalette(newDib, this->palette);
		}
		FreeImage_SetDotsPerMeterX(newDib, dpiToDpm(this->dpiX));
		FreeImage_SetDotsPerMeterY(newDib, dpiToDpm(this->dpiY));
		return S_OK;
	}

	/**
	 * Converts a source to the pixel format of the frame and gathers the palette.
	 * Indexed formats take the palette of the source unless one was set. A
	 * conversion to an indexed format generates a palette for the image.
	 *
	 * @param[in] input - image to write
	 * @param[in] convert - `true` if `input` has another pixel format than the frame
	 * @param[in,out] output - image in the pixel format of the frame
	 * @return result code
	 */
	HRESULT prepareSource(IWICBitmapSource * input, bool convert, ComPtr<IWICBitmapSource> & output) {
		ComPtr<IWICImagingFactory> factory;
		HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
		if ( FAILED(hr) ) {
			return hr;
		}
		ComPtr<IWICPalette> wicPalette;
		if ( this->info->indexed ) {
			hr = factory->CreatePalette(wicPalette.GetAddressOf());
			if ( SUCCEEDED(hr) ) {
				hr = convert ? wicPalette->InitializeFromBitmap(input, 1u << this->info->bpp, FALSE) : input->CopyPalette(wicPalette.Get());
			}
			if ( SUCCEEDED(hr) ) {
				hr = readPalette(wicPalette.Get(), &(this->palette));
			}
			if ( FAILED(hr) ) {
				return hr;
			}
		}
		if ( ! convert ) {
			return S_OK;
		}
		ComPtr<IWICFormatConverter> converter;
		hr = factory->CreateFormatConverter(converter.GetAddressOf());
		if ( SUCCEEDED(hr) ) {
			hr = converter->Initialize(input, *this->info->format, WICBitmapDitherTypeNone, wicPalette.Get(), 0.0, WICBitmapPaletteTypeCustom);
		}
		if ( FAILED(hr) ) {
			return hr;
		}
		output.Reset();
		return converter->QueryInterface(__uuidof(IWICBitmapSource), reinterpret_cast<void **>(output.GetAddressOf()));
	}
};


/**
 * WIC encoder of one FreeImage format.
 */
class Encoder : public ComObject<IWICBitmapEncoder> {
private:
	FREE_IMAGE_FORMAT fif; /**< Encoded FreeImage format. */
	GUID container; /**< Container format reported to WIC. */
	CriticalSection lock; /**< Guards the members below. */
	ComPtr<IStream> stream; /**< Destination stream. */
	ComPtr<IWICBitmapFrameEncode> frame; /**< The frame handed out, a `FrameEncode`. */
	bool committed; /**< `true` after `Commit`. */
public:
	/**
	 * Constructor.
	 *
	 * @param[in] aFif - FreeImage format to encode
	 */
	explicit Encoder(FREE_IMAGE_FORMAT aFif):
		ComObject<IWICBitmapEncoder>(),
		fif(aFif),
		container(formatGuid(FIWIC_GUID_CONTAINER, aFif)),
		lock(),
		stream(),
		frame(),
		committed(false)
	{}

	/** Destructor. */
	~Encoder() noexcept override = default;

	STDMETHOD(Initialize)(IStream * pIStream, WICBitmapEncoderCacheOption cacheOption) noexcept override {
		if (pIStream == nullptr) {
			return E_POINTER;
		}
		Guard guard(this->lock);
		if ( this->stream ) {
			return WINCODEC_ERR_WRONGSTATE;
		}
		this->stream = pIStream;
		return S_OK;
	}

	STDMETHOD(GetContainerFormat)(GUID * pguidContainerFormat) noexcept override {
		if (pguidContainerFormat == nullptr) {
			return E_POINTER;
		}
		*pguidContainerFormat = this->container;
		return S_OK;
	}

	STDMETHOD(GetEncoderInfo)(IWICBitmapEncoderInfo ** ppIEncoderInfo) noexcept override {
		if (ppIEncoderInfo == nullptr) {
			return E_POINTER;
		}
		return createComponentInfo(formatGuid(FIWIC_GUID_ENCODER, this->fif), __uuidof(IWICBitmapEncoderInfo), reinterpret_cast<void **>(ppIEncoderInfo));
	}

	STDMETHOD(SetColorContexts)(UINT cCount, IWICColorContext ** ppIColorContext) noexcept override {
		return WINCODEC_ERR_UNSUPPORTEDOPERATION;
	}

	STDMETHOD(SetPalette)(IWICPalette * pIPalette) noexcept override {
		return WINCODEC_ERR_UNSUPPORTEDOPERATION;
	}

	STDMETHOD(SetThumbnail)(IWICBitmapSource * pIThumbnail) noexcept override {
		return WINCODEC_ERR_UNSUPPORTEDOPERATION;
	}

	STDMETHOD(SetPreview)(IWICBitmapSource * pIPreview) noexcept override {
		return WINCODEC_ERR_UNSUPPORTEDOPERATION;
	}

	STDMETHOD(CreateNewFrame)(IWICBitmapFrameEncode ** ppIFrameEncode, IPropertyBag2 ** ppIEncoderOptions) noexcept override {
		if (ppIFrameEncode == nullptr) {
			return E_POINTER;
		}
		*ppIFrameEncode = nullptr;
		if (ppIEncoderOptions != nullptr) {
			*ppIEncoderOptions = nullptr;
		}
		Guard guard(this->lock);
		if ( ! this->stream ) {
			return WINCODEC_ERR_NOTINITIALIZED;
		}
		if (this->frame || this->committed) {
			/* The supported formats store a single image. */
			return WINCODEC_ERR_UNSUPPORTEDOPERATION;
		}
		FrameEncode * newFrame = new(std::nothrow) FrameEncode(this->fif);
		if (newFrame == nullptr) {
			return E_OUTOFMEMORY;
		}
		if (ppIEncoderOptions != nullptr) {
			PropertyBag * bag = new(std::nothrow) PropertyBag();
			if (bag == nullptr) {
				newFrame->Release();
				return E_OUTOFMEMORY;
			}
			*ppIEncoderOptions = bag;
		}
		this->frame.Attach(newFrame);
		newFrame->AddRef();
		*ppIFrameEncode = newFrame;
		return S_OK;
	}

	STDMETHOD(Commit)() noexcept override {
		Guard guard(this->lock);
		if ( ! this->stream ) {
			return WINCODEC_ERR_NOTINITIALIZED;
		}
		if ( this->committed ) {
			return WINCODEC_ERR_WRONGSTATE;
		}
		if ( ! this->frame ) {
			return WINCODEC_ERR_FRAMEMISSING;
		}
		const Dib dib = static_cast<FrameEncode *>(this->frame.Get())->bitmap();
		if ( ! dib ) {
			/* The frame has to be committed first. */
			return WINCODEC_ERR_WRONGSTATE;
		}
		try {
			FreeImageIO io;
			initIo(&io);
			if ( ! FreeImage_SaveToHandle(this->fif, dib.get(), &io, this->stream.Get(), 0) ) {
				return E_FAIL;
			}
			this->stream->Commit(STGC_DEFAULT);
			this->committed = true;
			return S_OK;
		}
		FIWIC_CATCH_ALL
	}

	STDMETHOD(GetMetadataQueryWriter)(IWICMetadataQueryWriter ** ppIMetadataQueryWriter) noexcept override {
		return WINCODEC_ERR_UNSUPPORTEDOPERATION;
	}
};


HRESULT createEncoder(FREE_IMAGE_FORMAT fif, REFIID riid, void ** ppv) {
	if (ppv == nullptr) {
		return E_POINTER;
	}
	*ppv = nullptr;
	if ( ! isEncoderFormat(fif) ) {
		return CLASS_E_CLASSNOTAVAILABLE;
	}
	Encoder * encoder = new(std::nothrow) Encoder(fif);
	if (encoder == nullptr) {
		return E_OUTOFMEMORY;
	}
	const HRESULT hr = encoder->QueryInterface(riid, ppv);
	encoder->Release();
	return hr;
}
