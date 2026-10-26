/**
 * @file fiwicutil.hpp
 * @author Daniel Starke
 * @date 2024-12-03
 * @version 2026-10-26
 *
 * Shared infrastructure of the FreeImage WIC codec.
 */
#ifndef __FIWICUTIL_HPP__
#define __FIWICUTIL_HPP__

#include <windows.h>
#include <objbase.h>
#include <oleidl.h>
#include <ocidl.h>
#include <propidl.h>
#include <wincodec.h>
#include <unknwn.h>
#include <memory>
#include <new>
#include <utility>
#include <string>
#include <vector>
#include <FreeImage.h>
/* Utility.hpp provides the ComPtr implementation used by this project. The
 * system <wrl/client.h> must not be included because it defines the same
 * Microsoft::WRL entities. */
#include <Utility.hpp>
#include <fiwic.h>


using Microsoft::WRL::ComPtr;


/** Converts an allocation failure into the matching result code. */
#define FIWIC_CATCH_ALL \
	catch (const std::bad_alloc &) { \
		return E_OUTOFMEMORY; \
	} catch (...) { \
		return E_FAIL; \
	}


/* module state ***************************************************************/


/**
 * Returns the module handle of this DLL.
 *
 * @return module handle
 */
HMODULE moduleHandle() noexcept;


/**
 * `DllMain` implementation of fiwic.dll.
 *
 * @param[in] hInstance - module handle
 * @param[in] reason - attachment reason
 * @param[in] reserved - unload flag
 * @return `TRUE`
 */
extern "C" BOOL WINAPI dllMain(HINSTANCE hInstance, DWORD reason, LPVOID reserved) noexcept;


/**
 * Initializes the FreeImage library once.
 * Must not be called from `DllMain`.
 *
 * @return `true` if FreeImage is usable
 */
bool ensureFreeImage() noexcept;


/** Registers the creation of one COM object. */
void objectCreated() noexcept;


/** Registers the destruction of one COM object. */
void objectDestroyed() noexcept;


/**
 * Returns the number of live COM objects.
 *
 * @return object count
 */
LONG objectCount() noexcept;


/**
 * Locks or unlocks the module in memory (`IClassFactory::LockServer`).
 *
 * @param[in] lock - `true` to lock, `false` to unlock
 */
void lockServer(bool lock) noexcept;


/**
 * Returns the number of server locks.
 *
 * @return lock count
 */
LONG lockCount() noexcept;


/** Critical section. Use `Guard` to hold it. */
class CriticalSection {
private:
	CRITICAL_SECTION criticalSection; /**< Wrapped critical section. */
public:
	/** Constructor. */
	CriticalSection() noexcept {
		InitializeCriticalSection(&(this->criticalSection));
	}

	/** Destructor. */
	~CriticalSection() noexcept {
		DeleteCriticalSection(&(this->criticalSection));
	}

	/** Not copyable. */
	CriticalSection(const CriticalSection &) = delete;

	/** Not assignable. */
	CriticalSection & operator= (const CriticalSection &) = delete;

	/** Enters the critical section. */
	void enter() noexcept {
		EnterCriticalSection(&(this->criticalSection));
	}

	/** Leaves the critical section. */
	void leave() noexcept {
		LeaveCriticalSection(&(this->criticalSection));
	}
};


/** Holds a critical section for the lifetime of the object. */
class Guard {
private:
	CriticalSection & criticalSection; /**< Held critical section. */
public:
	/**
	 * Constructor.
	 *
	 * @param[in] cs - critical section to enter
	 */
	explicit Guard(CriticalSection & cs) noexcept:
		criticalSection(cs)
	{
		this->criticalSection.enter();
	}

	/** Destructor. */
	~Guard() noexcept {
		this->criticalSection.leave();
	}

	/** Not copyable. */
	Guard(const Guard &) = delete;

	/** Not assignable. */
	Guard & operator= (const Guard &) = delete;
};


/* formats ********************************************************************/


/** Component kinds identified by derived GUIDs. */
enum GuidKind {
	FIWIC_GUID_DECODER, /**< Class ID of the decoder. */
	FIWIC_GUID_ENCODER, /**< Class ID of the encoder. */
	FIWIC_GUID_CONTAINER /**< Container format. */
};


/** One byte pattern WIC uses to recognize a file format. */
struct Pattern {
	DWORD position; /**< Offset of the pattern from the stream start. */
	std::vector<BYTE> pattern; /**< Expected bytes. */
	std::vector<BYTE> mask; /**< Bits of `pattern` which are compared. */
};


/**
 * Checks if Windows provides a decoder and encoder for a FreeImage format.
 * Such formats are not registered to avoid competing with the system codecs.
 *
 * @param[in] fif - FreeImage format
 * @return `true` for BMP, ICO, JPEG, PNG, TIFF, GIF, DDS, JPEG XR, WebP and RAW
 */
bool isNativeFormat(FREE_IMAGE_FORMAT fif) noexcept;


/**
 * Checks if a FreeImage format can hold multiple pages.
 *
 * @param[in] fif - FreeImage format
 * @return `true` for the formats supported by the FreeImage multi page API: GIF, ICO and TIFF
 */
bool isMultiPageFormat(FREE_IMAGE_FORMAT fif) noexcept;


/**
 * Checks if the codec provides a decoder for a FreeImage format.
 *
 * @param[in] fif - FreeImage format
 * @return `true` if the format is enabled, readable and not native
 */
bool isDecoderFormat(FREE_IMAGE_FORMAT fif) noexcept;


/**
 * Checks if the codec provides an encoder for a FreeImage format.
 *
 * @param[in] fif - FreeImage format
 * @return `true` if the format is enabled, writable and not native
 */
bool isEncoderFormat(FREE_IMAGE_FORMAT fif) noexcept;


/**
 * Derives the GUID of a component for a FreeImage format.
 * Decoders for formats without file signature sort behind all others.
 *
 * @param[in] kind - component kind
 * @param[in] fif - FreeImage format
 * @return derived GUID
 */
GUID formatGuid(GuidKind kind, FREE_IMAGE_FORMAT fif) noexcept;


/**
 * Reverses `formatGuid`.
 *
 * @param[in] kind - component kind
 * @param[in] guid - derived GUID
 * @param[out] fif - receiving FreeImage format
 * @return `true` if `guid` is derived from the base of `kind`
 */
bool parseFormatGuid(GuidKind kind, const GUID & guid, FREE_IMAGE_FORMAT * fif) noexcept;


/**
 * Returns the byte patterns identifying a FreeImage format.
 * Formats without a reliable signature get a pattern matching every stream.
 *
 * @param[in] fif - FreeImage format
 * @return patterns (never empty)
 */
std::vector<Pattern> formatPatterns(FREE_IMAGE_FORMAT fif);


/**
 * Returns the extension list of a FreeImage format.
 *
 * @param[in] fif - FreeImage format
 * @param[in] prefix - prepended to every extension (e.g. `.` or `*.`)
 * @param[in] sep - separator between extensions
 * @return `.A,.B` style extension list
 */
std::wstring fifExtensions(FREE_IMAGE_FORMAT fif, const wchar_t * prefix, wchar_t sep);


/**
 * Checks if a list of extensions contains a file name's extension.
 *
 * @param[in] extensions - extension list separated by `,` or `;` with optional `*.` or `.` prefixes
 * @param[in] fileName - file name or path
 * @return `true` on a match
 */
bool extensionMatches(const wchar_t * extensions, const std::wstring & fileName);


/* pixel formats **************************************************************/


/** Relation between a WIC pixel format and its FreeImage representation. */
struct PixelInfo {
	const GUID * format; /**< WIC pixel format. */
	FREE_IMAGE_TYPE type; /**< FreeImage bitmap type. */
	unsigned bpp; /**< Bits per pixel. */
	bool indexed; /**< `true` if the pixels index a palette. */
	int fallback[8]; /**< Table indices of substitutes in order of preference, terminated by `-1`. */
};


/**
 * Looks up a WIC pixel format.
 *
 * @param[in] format - WIC pixel format
 * @return table entry or `nullptr` if the codec does not use the format
 */
const PixelInfo * findPixelInfo(const GUID & format) noexcept;


/**
 * Iterates over the pixel formats known to the codec.
 *
 * @param[in] index - table index
 * @return table entry or `nullptr` after the last entry
 */
const PixelInfo * pixelInfoAt(size_t index) noexcept;


/**
 * Selects the pixel format an encoder uses for a requested one.
 *
 * @param[in] fif - FreeImage format to encode
 * @param[in] requested - requested WIC pixel format
 * @return closest pixel format supported by the encoder or `nullptr`
 */
const PixelInfo * negotiatePixelFormat(FREE_IMAGE_FORMAT fif, const GUID & requested) noexcept;


/* bitmaps and streams ********************************************************/


/** Shared ownership of a FreeImage bitmap. */
typedef std::shared_ptr<FIBITMAP> Dib;


/**
 * Takes ownership of a FreeImage bitmap.
 *
 * @param[in] dib - bitmap to own (may be `nullptr`)
 * @return owning pointer
 */
inline Dib makeDib(FIBITMAP * dib) {
	return Dib(dib, [](FIBITMAP * p) { FreeImage_Unload(p); });
}


/**
 * Converts a UTF-8 string to a wide string.
 *
 * @param[in] s - UTF-8 string (may be `nullptr`)
 * @return converted string
 */
std::wstring toWide(const char * s);


/**
 * Converts the ASCII letters of a string to upper case.
 *
 * @param[in,out] s - string to convert
 */
void upperCase(std::wstring & s) noexcept;


/**
 * Creates a seekable memory copy of a stream.
 *
 * @param[in] stream - source stream, copied from its start
 * @param[out] out - resulting memory stream positioned at its start
 * @return result code
 * @remarks Takes the pointer by reference. `&out` of a `ComPtr` would release it.
 */
HRESULT streamAsMemory(IStream * stream, ComPtr<IStream> & out);


/**
 * Fills a FreeImage I/O descriptor which drives an `IStream`.
 *
 * The FreeImage handle to use with it is the `IStream` pointer.
 *
 * @param[out] io - descriptor to fill
 */
void initIo(FreeImageIO * io) noexcept;


/**
 * Checks if a stream holds an image of a FreeImage format.
 * The content is checked first. Formats without a signature are accepted if
 * the file name of the stream carries one of the extensions of the format.
 * The stream position is preserved.
 *
 * @param[in] fif - FreeImage format
 * @param[in] stream - stream to check
 * @return `true` if the stream is likely to be decodable
 */
bool validateStream(FREE_IMAGE_FORMAT fif, IStream * stream);


/**
 * Creates the WIC component info object of a registered component.
 *
 * @param[in] clsid - class ID of the component
 * @param[in] riid - requested interface
 * @param[out] ppv - receiving interface pointer
 * @return result code
 */
HRESULT createComponentInfo(REFCLSID clsid, REFIID riid, void ** ppv);


/**
 * Creates a metadata query reader over a FreeImage bitmap's metadata.
 *
 * @param[in] dib - bitmap to read metadata from (may be `nullptr`)
 * @param[in] container - container format reported by the reader
 * @param[in] top - `true` for the top level reader
 * @param[out] out - created reader
 * @return result code
 */
HRESULT createMetadataQueryReader(FIBITMAP * dib, const GUID & container, bool top, IWICMetadataQueryReader ** out);


/* components *****************************************************************/


/**
 * Creates the decoder for a FreeImage format.
 *
 * @param[in] fif - FreeImage format
 * @param[in] riid - requested interface
 * @param[out] ppv - receiving interface pointer
 * @return result code
 */
HRESULT createDecoder(FREE_IMAGE_FORMAT fif, REFIID riid, void ** ppv);


/**
 * Creates the encoder for a FreeImage format.
 *
 * @param[in] fif - FreeImage format
 * @param[in] riid - requested interface
 * @param[out] ppv - receiving interface pointer
 * @return result code
 */
HRESULT createEncoder(FREE_IMAGE_FORMAT fif, REFIID riid, void ** ppv);


/**
 * Registers all components and the shell integration.
 *
 * @param[in] perUser - `true` to register for the current user only; otherwise
 * the registration is machine wide if permitted and per
 * user if not
 * @return `S_OK` or the error as `HRESULT`
 */
HRESULT registerServer(bool perUser);


/**
 * Removes everything `registerServer` has added.
 *
 * @return `S_OK` or the error as `HRESULT`
 */
HRESULT unregisterServer();


/* COM plumbing ***************************************************************/


/**
 * Reference counting and `IUnknown` plumbing for one or more COM interfaces.
 * The class derives from the published interfaces only. Each of them brings its
 * own `IUnknown` base and the single set of `IUnknown` methods defined here
 * overrides all of them. A separate `IUnknown` implementing base class would
 * leave the `IUnknown` methods of the interface bases pure.
 * Interfaces inherited by a published interface (e.g. `IWICBitmapSource` of
 * `IWICBitmapFrameDecode`) are published by overriding `provideInterface`.
 *
 * @tparam Interface - primary published interface
 * @tparam Others - further published interfaces
 */
/* COM interfaces have no virtual destructor by design. Objects are destroyed
 * through `Release` only. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
template <typename Interface, typename... Others>
class ComObject : public Interface, public Others... {
private:
	LONG refCount; /**< References to this object. */
public:
	STDMETHOD(QueryInterface)(REFIID riid, void ** ppv) noexcept {
		if (ppv == nullptr) {
			return E_POINTER;
		}
		*ppv = nullptr;
		if (( ! this->provideInterface(riid, ppv) ) || *ppv == nullptr) {
			*ppv = nullptr;
			return E_NOINTERFACE;
		}
		this->AddRef();
		return S_OK;
	}

	STDMETHOD_(ULONG, AddRef)() noexcept {
		return ULONG(InterlockedIncrement(&(this->refCount)));
	}

	STDMETHOD_(ULONG, Release)() noexcept {
		const LONG count = InterlockedDecrement(&(this->refCount));
		if (count == 0) {
			delete this;
			/* Only after the destruction: DllCanUnloadNow must not report the module as
			 * unused while object code is still running. */
			objectDestroyed();
			return 0;
		}
		return ULONG(count);
	}
protected:
	/** Constructor. */
	ComObject() noexcept:
		refCount(1)
	{
		objectCreated();
	}

	/** Destructor. The object count is released by `Release` after the object is gone. */
	virtual ~ComObject() noexcept {}

	/** Not copyable. */
	ComObject(const ComObject &) = delete;

	/** Not assignable. */
	ComObject & operator= (const ComObject &) = delete;

	/**
	 * Resolves an interface ID to the matching interface pointer.
	 * Derived classes extend the published interface set and fall back to
	 * this implementation.
	 *
	 * @param[in] riid - requested interface ID
	 * @param[out] ppv - receiving pointer
	 * @return `true` when the interface is supported
	 */
	virtual bool provideInterface(REFIID riid, void ** ppv) noexcept {
		if (riid == __uuidof(IUnknown)) {
			/* Always the primary interface to keep the COM identity stable. */
			*ppv = static_cast<IUnknown *>(static_cast<Interface *>(this));
			return true;
		}
		return this->provideListed(riid, ppv, InterfaceList<Interface, Others...>());
	}
private:
	/**
	 * Type list tag driving the interface lookup recursion.
	 *
	 * @tparam Interfaces - remaining interfaces
	 */
	template <typename... Interfaces>
	struct InterfaceList {};

	/**
	 * Terminates the interface lookup recursion.
	 *
	 * @return `false`
	 */
	bool provideListed(REFIID, void **, InterfaceList<>) noexcept {
		return false;
	}

	/**
	 * Resolves an interface ID against the listed interfaces.
	 *
	 * @param[in] riid - requested interface ID
	 * @param[out] ppv - receiving pointer
	 * @return `true` when the interface is listed
	 * @tparam First - interface to check
	 * @tparam Rest - remaining interfaces
	 */
	template <typename First, typename... Rest>
	bool provideListed(REFIID riid, void ** ppv, InterfaceList<First, Rest...>) noexcept {
		if (riid == __uuidof(First)) {
			*ppv = static_cast<First *>(this);
			return true;
		}
		return this->provideListed(riid, ppv, InterfaceList<Rest...>());
	}
};
#pragma GCC diagnostic pop


#endif /* __FIWICUTIL_HPP__ */
