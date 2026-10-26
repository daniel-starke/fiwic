/**
 * @file fiwicreg.cpp
 * @author Daniel Starke
 * @date 2024-12-03
 * @version 2026-10-26
 *
 * Registration of the WIC components and of the shell integration.
 *
 * The registry keys are written below `HKEY_LOCAL_MACHINE\Software\Classes`.
 * If this is not permitted, `HKEY_CURRENT_USER\Software\Classes` is used.
 *
 * Shell integration enables Explorer thumbnails for the extensions of all
 * registered decoders by pointing them to the system photo thumbnail
 * provider. Each value is only written if the extension has no handler yet.
 * The written values are recorded to remove exactly them on unregistration.
 */
#include <string.h>
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <string>
#include <vector>
#include <FreeImage.h>
#include <fiwicutil.hpp>


/** `CATID_WICBitmapDecoders`. */
static const wchar_t catIdDecoders[] = L"{7ED96837-96F0-4812-B211-F13C24117ED3}";

/** `CATID_WICBitmapEncoders`. */
static const wchar_t catIdEncoders[] = L"{AC757296-3522-4E11-9862-C17BE5A1767E}";

/** Shell extension key of `IThumbnailProvider` handlers. */
static const wchar_t thumbnailHandlerKey[] = L"ShellEx\\{E357FCCD-A995-4576-B01F-234630154E96}";

/** The Windows photo thumbnail provider. */
static const wchar_t photoThumbnailProvider[] = L"{C7657C4A-9F68-40FA-A4DF-96BC08EB3551}";

/** Maps file extensions to a kind of the Windows property system. */
static const wchar_t photoViewerProgId[] = L"PhotoViewer.FileAssoc.Tiff";

/** Windows Photo Viewer: file extensions it offers to open. */
static const wchar_t photoViewerKey[] = L"SOFTWARE\\Microsoft\\Windows Photo Viewer\\Capabilities\\FileAssociations";

/** Windows Photo Viewer: key which tells that it is installed. */
static const wchar_t photoViewerCapabilitiesKey[] = L"SOFTWARE\\Microsoft\\Windows Photo Viewer\\Capabilities";

/** Explorer: picture kind of extensions. */
static const wchar_t kindMapKey[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\KindMap";

/** Key below `Classes` which holds the registration bookkeeping. */
static const wchar_t progIdKey[] = FIWIC_PROGID;

/**
 * Key below the ProgID with one value per architecture whose registration exists.
 * The ProgID and the shell integration are shared by the 32 bit and 64 bit
 * registry views, so they are only removed with the last registration.
 */
static const wchar_t viewsKey[] = FIWIC_PROGID L"\\Views";

/** Name of the value in `viewsKey` which belongs to this build. */
#ifdef _WIN64
static const wchar_t viewName[] = L"x64";
#else
static const wchar_t viewName[] = L"x86";
#endif


/** Marks written for an extension. */
enum ShellMark {
	FIWIC_MARK_THUMBNAIL = 1, /**< Thumbnail provider handler. */
	FIWIC_MARK_PERCEIVED = 2, /**< `PerceivedType` value. */
	FIWIC_MARK_CONTENT = 4, /**< `Content Type` value. */
	FIWIC_MARK_KIND = 8, /**< `KindMap` value (machine wide only). */
	FIWIC_MARK_OPENWITH = 16, /**< Windows Photo Viewer in `OpenWithProgids`. */
	FIWIC_MARK_VIEWER = 32 /**< Windows Photo Viewer file association (machine wide only). */
};


/**
 * Returns the key path below the root for a key below `Classes`.
 *
 * @param[in] key - key below `Classes`
 * @return key path
 */
static std::wstring classesKey(const std::wstring & key) {
	return L"Software\\Classes\\" + key;
}


/**
 * Returns the registry representation of a GUID.
 *
 * @param[in] guid - GUID
 * @return `{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}`
 */
static std::wstring guidString(const GUID & guid) {
	wchar_t buffer[40];
	if (StringFromGUID2(guid, buffer, 40) == 0) {
		return std::wstring();
	}
	return std::wstring(buffer);
}


/** Writes values below one registry root and keeps the first error. */
class RegWriter {
private:
	HKEY root; /**< Registry root. */
	REGSAM wow; /**< Registry view flag (`KEY_WOW64_64KEY` or `0`). */
	LONG status; /**< First error or `ERROR_SUCCESS`. */
public:
	/**
	 * Constructor.
	 *
	 * @param[in] aRoot - registry root
	 * @param[in] aWow - registry view flag (`KEY_WOW64_64KEY` or `0`)
	 */
	explicit RegWriter(HKEY aRoot, REGSAM aWow = 0) noexcept:
		root(aRoot),
		wow(aWow),
		status(ERROR_SUCCESS)
	{}

	/**
	 * Returns the first error.
	 *
	 * @return `ERROR_SUCCESS` if all writes succeeded
	 */
	LONG firstError() const noexcept {
		return this->status;
	}

	/**
	 * Creates a key.
	 *
	 * @param[in] key - key path below the root
	 */
	void key(const std::wstring & key) {
		if (this->status != ERROR_SUCCESS) {
			return;
		}
		HKEY handle = nullptr;
		const LONG res = RegCreateKeyExW(this->root, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE | this->wow, nullptr, &handle, nullptr);
		if (res == ERROR_SUCCESS) {
			RegCloseKey(handle);
		} else {
			this->status = res;
		}
	}

	/**
	 * Writes a string value.
	 *
	 * @param[in] key - key path below the root
	 * @param[in] name - value name (`nullptr` for the default value)
	 * @param[in] value - string
	 */
	void string(const std::wstring & key, const wchar_t * name, const std::wstring & value) {
		this->set(key, name, REG_SZ, value.c_str(), DWORD((value.size() + 1) * sizeof(wchar_t)));
	}

	/**
	 * Writes a DWORD value.
	 *
	 * @param[in] key - key path below the root
	 * @param[in] name - value name
	 * @param[in] value - number
	 */
	void dword(const std::wstring & key, const wchar_t * name, DWORD value) {
		this->set(key, name, REG_DWORD, &value, sizeof(value));
	}

	/**
	 * Writes a binary value.
	 *
	 * @param[in] key - key path below the root
	 * @param[in] name - value name
	 * @param[in] value - bytes
	 */
	void binary(const std::wstring & key, const wchar_t * name, const std::vector<BYTE> & value) {
		static const BYTE empty = 0;
		this->set(key, name, REG_BINARY, value.empty() ? &empty : &value[0], DWORD(value.size()));
	}
private:
	/**
	 * Writes one value.
	 *
	 * @param[in] key - key path below the root
	 * @param[in] name - value name (`nullptr` for the default value)
	 * @param[in] type - value type
	 * @param[in] data - value data
	 * @param[in] size - size of `data` in bytes
	 */
	void set(const std::wstring & key, const wchar_t * name, DWORD type, const void * data, DWORD size) {
		if (this->status != ERROR_SUCCESS) {
			return;
		}
		HKEY handle = nullptr;
		LONG res = RegCreateKeyExW(this->root, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE | this->wow, nullptr, &handle, nullptr);
		if (res == ERROR_SUCCESS) {
			res = RegSetValueExW(handle, name, 0, type, static_cast<const BYTE *>(data), size);
			RegCloseKey(handle);
		}
		if (res != ERROR_SUCCESS) {
			this->status = res;
		}
	}
};


/**
 * Checks if a value exists.
 *
 * @param[in] root - registry root
 * @param[in] key - key path below the root
 * @param[in] name - value name (`nullptr` for the default value)
 * @param[in] wow - registry view flag (`KEY_WOW64_64KEY` or `0`)
 * @return `true` if the value exists
 */
static bool valueExists(HKEY root, const std::wstring & key, const wchar_t * name, REGSAM wow = 0) {
	HKEY handle = nullptr;
	if (RegOpenKeyExW(root, key.c_str(), 0, KEY_QUERY_VALUE | wow, &handle) != ERROR_SUCCESS) {
		return false;
	}
	const LONG res = RegQueryValueExW(handle, name, nullptr, nullptr, nullptr, nullptr);
	RegCloseKey(handle);
	return res == ERROR_SUCCESS;
}


/**
 * Reads a string value.
 *
 * @param[in] root - registry root
 * @param[in] key - key path below the root
 * @param[in] name - value name (`nullptr` for the default value)
 * @param[in] wow - registry view flag (`KEY_WOW64_64KEY` or `0`)
 * @return string value or an empty string
 */
static std::wstring readString(HKEY root, const std::wstring & key, const wchar_t * name, REGSAM wow = 0) {
	std::wstring res;
	HKEY handle = nullptr;
	if (RegOpenKeyExW(root, key.c_str(), 0, KEY_QUERY_VALUE | wow, &handle) != ERROR_SUCCESS) {
		return res;
	}
	wchar_t buffer[MAX_PATH];
	DWORD type = 0;
	DWORD size = sizeof(buffer) - sizeof(wchar_t);
	if (RegQueryValueExW(handle, name, nullptr, &type, reinterpret_cast<BYTE *>(buffer), &size) == ERROR_SUCCESS && type == REG_SZ) {
		buffer[size / sizeof(wchar_t)] = L'\0';
		res = buffer;
	}
	RegCloseKey(handle);
	return res;
}


/**
 * Deletes a key if it has neither values nor sub keys.
 *
 * @param[in] root - registry root
 * @param[in] key - key path below the root
 */
static void deleteKeyIfEmpty(HKEY root, const std::wstring & key) {
	HKEY handle = nullptr;
	if (RegOpenKeyExW(root, key.c_str(), 0, KEY_QUERY_VALUE, &handle) != ERROR_SUCCESS) {
		return;
	}
	DWORD subKeys = 0;
	DWORD values = 0;
	const LONG res = RegQueryInfoKeyW(handle, nullptr, nullptr, nullptr, &subKeys, nullptr, nullptr, &values, nullptr, nullptr, nullptr, nullptr);
	RegCloseKey(handle);
	if (res == ERROR_SUCCESS && subKeys == 0 && values == 0) {
		RegDeleteKeyW(root, key.c_str());
	}
}


/**
 * Deletes a value.
 *
 * @param[in] root - registry root
 * @param[in] key - key path below the root
 * @param[in] name - value name
 * @param[in] wow - registry view flag (`KEY_WOW64_64KEY` or `0`)
 */
static void deleteValue(HKEY root, const std::wstring & key, const wchar_t * name, REGSAM wow = 0) {
	HKEY handle = nullptr;
	if (RegOpenKeyExW(root, key.c_str(), 0, KEY_SET_VALUE | wow, &handle) == ERROR_SUCCESS) {
		RegDeleteValueW(handle, name);
		RegCloseKey(handle);
	}
}


/**
 * Reads a DWORD value.
 *
 * @param[in] root - registry root
 * @param[in] key - key path below the root
 * @param[in] name - value name
 * @return value or `0`
 */
static DWORD readDword(HKEY root, const std::wstring & key, const wchar_t * name) {
	DWORD res = 0;
	HKEY handle = nullptr;
	if (RegOpenKeyExW(root, key.c_str(), 0, KEY_QUERY_VALUE, &handle) == ERROR_SUCCESS) {
		DWORD type = 0;
		DWORD size = sizeof(res);
		if (RegQueryValueExW(handle, name, nullptr, &type, reinterpret_cast<BYTE *>(&res), &size) != ERROR_SUCCESS || type != REG_DWORD) {
			res = 0;
		}
		RegCloseKey(handle);
	}
	return res;
}


/**
 * Counts the values of a key.
 *
 * @param[in] root - registry root
 * @param[in] key - key path below the root
 * @return number of values (`0` if the key does not exist)
 */
static DWORD countValues(HKEY root, const std::wstring & key) {
	DWORD values = 0;
	HKEY handle = nullptr;
	if (RegOpenKeyExW(root, key.c_str(), 0, KEY_QUERY_VALUE, &handle) == ERROR_SUCCESS) {
		if (RegQueryInfoKeyW(handle, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &values, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) {
			values = 0;
		}
		RegCloseKey(handle);
	}
	return values;
}


/**
 * Registers the decoder or encoder of a FreeImage format.
 *
 * @param[in,out] reg - registry writer
 * @param[in] module - path of this DLL
 * @param[in] fif - FreeImage format
 * @param[in] decoder - `true` for the decoder, `false` for the encoder
 */
static void registerComponent(RegWriter & reg, const std::wstring & module, FREE_IMAGE_FORMAT fif, bool decoder) {
	const std::wstring id = guidString(formatGuid(decoder ? FIWIC_GUID_DECODER : FIWIC_GUID_ENCODER, fif));
	const std::wstring key = classesKey(L"CLSID\\" + id);
	std::wstring format = toWide(FreeImage_GetFormatFromFIF(fif));
	if (fif == FIF_PBM || fif == FIF_PGM || fif == FIF_PPM) {
		format += L" (ASCII)";
	} else if (fif == FIF_PBMRAW || fif == FIF_PGMRAW || fif == FIF_PPMRAW) {
		format.erase(format.size() - 3);
		format += L" (binary)";
	}
	const std::wstring name = L"FreeImage " + format + (decoder ? L" Decoder" : L" Encoder");
	const std::wstring description = toWide(FreeImage_GetFIFDescription(fif));
	const std::wstring mime = toWide(FreeImage_GetFIFMimeType(fif));
	const bool multiPage = isMultiPageFormat(fif);
	const bool lossy = fif == FIF_J2K || fif == FIF_JP2 || fif == FIF_JNG;
	reg.string(key, nullptr, name);
	reg.string(key, L"FriendlyName", name);
	reg.string(key, L"Author", FIWIC_VENDOR_NAME);
	reg.string(key, L"Description", description.empty() ? name : description);
	reg.string(key, L"ContainerFormat", guidString(formatGuid(FIWIC_GUID_CONTAINER, fif)));
	reg.string(key, L"FileExtensions", fifExtensions(fif, L".", L','));
	if ( ! mime.empty() ) {
		reg.string(key, L"MimeTypes", mime);
	}
	reg.string(key, L"Vendor", guidString(GUID_Vendor_FIWIC));
	reg.string(key, L"Version", FIWIC_REG_VERSION);
	reg.string(key, L"SpecVersion", FIWIC_REG_VERSION);
	reg.dword(key, L"SupportAnimation", (decoder && fif == FIF_GIF) ? 1 : 0);
	reg.dword(key, L"SupportChromaKey", 0);
	reg.dword(key, L"SupportLossless", lossy ? 0 : 1);
	reg.dword(key, L"SupportMultiframe", (decoder && multiPage) ? 1 : 0);
	/* Pixel formats which the component produces or accepts. */
	std::vector<const GUID *> formats;
	for (size_t i = 0; pixelInfoAt(i) != nullptr; ++i) {
		const PixelInfo * info = pixelInfoAt(i);
		const PixelInfo * used = decoder ? info : negotiatePixelFormat(fif, *info->format);
		if (used == nullptr) {
			continue;
		}
		bool known = false;
		for (size_t j = 0; j < formats.size(); ++j) {
			known = known || formats[j] == used->format;
		}
		if ( ! known ) {
			formats.push_back(used->format);
		}
	}
	for (size_t i = 0; i < formats.size(); ++i) {
		reg.key(key + L"\\Formats\\" + guidString(*formats[i]));
	}
	if ( decoder ) {
		const std::vector<Pattern> patterns = formatPatterns(fif);
		for (size_t i = 0; i < patterns.size(); ++i) {
			wchar_t index[16];
			wsprintfW(index, L"%u", unsigned(i));
			const std::wstring patternKey = key + L"\\Patterns\\" + index;
			reg.dword(patternKey, L"Position", patterns[i].position);
			reg.dword(patternKey, L"Length", DWORD(patterns[i].pattern.size()));
			reg.binary(patternKey, L"Pattern", patterns[i].pattern);
			reg.binary(patternKey, L"Mask", patterns[i].mask);
		}
	}
	reg.string(key + L"\\InprocServer32", nullptr, module);
	reg.string(key + L"\\InprocServer32", L"ThreadingModel", L"Both");
	/* Announce the component to WIC. */
	const std::wstring instance = classesKey(std::wstring(L"CLSID\\") + (decoder ? catIdDecoders : catIdEncoders) + L"\\Instance\\" + id);
	reg.string(instance, L"CLSID", id);
	reg.string(instance, L"FriendlyName", name);
}


/**
 * Checks if an extension already has a thumbnail handler.
 *
 * @param[in] ext - extension with leading dot
 * @return `true` if a handler exists
 */
static bool hasThumbnailHandler(const std::wstring & ext) {
	if ( valueExists(HKEY_CLASSES_ROOT, ext + L"\\" + thumbnailHandlerKey, nullptr) ) {
		return true;
	}
	const std::wstring progId = readString(HKEY_CLASSES_ROOT, ext, nullptr);
	return ( ! progId.empty() ) && valueExists(HKEY_CLASSES_ROOT, progId + L"\\" + thumbnailHandlerKey, nullptr);
}


/**
 * Enables Explorer thumbnails and the picture kind for an extension.
 * The marks of the values to write are recorded before the values are written.
 * A failed write can then leave a mark without a value, which is harmless since
 * `unregisterShell` only removes values it recognizes. The reverse would leave
 * a value behind forever.
 *
 * @param[in] root - registry root of the registration
 * @param[in] ext - extension with leading dot
 * @param[in] mime - MIME type of the format (may be empty)
 */
static void registerShell(HKEY root, const std::wstring & ext, const std::wstring & mime) {
	DWORD marks = 0;
	if ( ! hasThumbnailHandler(ext) ) {
		marks |= FIWIC_MARK_THUMBNAIL;
	}
	if ( ! valueExists(HKEY_CLASSES_ROOT, ext, L"PerceivedType") ) {
		marks |= FIWIC_MARK_PERCEIVED;
	}
	if (( ! mime.empty() ) && ( ! valueExists(HKEY_CLASSES_ROOT, ext, L"Content Type") )) {
		marks |= FIWIC_MARK_CONTENT;
	}
	/* The 64 bit view: the 32 bit view is invisible to Explorer. Not available for
	 * per user registrations. */
	if (root == HKEY_LOCAL_MACHINE && ( ! valueExists(HKEY_LOCAL_MACHINE, kindMapKey, ext.c_str(), KEY_WOW64_64KEY) )) {
		marks |= FIWIC_MARK_KIND;
	}
	/* Windows Photo Viewer decodes through WIC, but only offers the extensions it knows. */
	if ( valueExists(HKEY_CLASSES_ROOT, photoViewerProgId, L"ImageOptionFlags") ) {
		if ( ! valueExists(HKEY_CLASSES_ROOT, ext + L"\\OpenWithProgids", photoViewerProgId) ) {
			marks |= FIWIC_MARK_OPENWITH;
		}
		if (root == HKEY_LOCAL_MACHINE && ( ! valueExists(HKEY_LOCAL_MACHINE, photoViewerKey, ext.c_str(), KEY_WOW64_64KEY) )
			&& valueExists(HKEY_LOCAL_MACHINE, photoViewerCapabilitiesKey, L"ApplicationName", KEY_WOW64_64KEY)) {
			marks |= FIWIC_MARK_VIEWER;
		}
	}
	if (marks == 0) {
		return;
	}
	/* Keep the marks of earlier registrations, e.g. by the other architecture. */
	const std::wstring shellKey = classesKey(std::wstring(progIdKey) + L"\\Shell");
	RegWriter record(root);
	record.dword(shellKey, ext.c_str(), readDword(root, shellKey, ext.c_str()) | marks);
	if (record.firstError() != ERROR_SUCCESS) {
		return;
	}
	if ((marks & FIWIC_MARK_THUMBNAIL) != 0) {
		RegWriter reg(root);
		reg.string(classesKey(ext + L"\\" + thumbnailHandlerKey), nullptr, photoThumbnailProvider);
	}
	if ((marks & FIWIC_MARK_PERCEIVED) != 0) {
		RegWriter reg(root);
		reg.string(classesKey(ext), L"PerceivedType", L"image");
	}
	if ((marks & FIWIC_MARK_CONTENT) != 0) {
		RegWriter reg(root);
		reg.string(classesKey(ext), L"Content Type", mime);
	}
	if ((marks & FIWIC_MARK_KIND) != 0) {
		RegWriter kind(HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY);
		kind.string(kindMapKey, ext.c_str(), L"picture");
	}
	if ((marks & FIWIC_MARK_OPENWITH) != 0) {
		RegWriter reg(root);
		reg.string(classesKey(ext + L"\\OpenWithProgids"), photoViewerProgId, std::wstring());
	}
	if ((marks & FIWIC_MARK_VIEWER) != 0) {
		RegWriter reg(HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY);
		reg.string(photoViewerKey, ext.c_str(), photoViewerProgId);
	}
}


/**
 * Checks if a name from the registration bookkeeping is a plain extension.
 * The bookkeeping lies in a key that users can write to. Anything which could
 * address another key is ignored.
 *
 * @param[in] ext - value name
 * @return `true` for `.name`
 */
static bool isPlainExtension(const std::wstring & ext) {
	return ext.size() > 1 && ext[0] == L'.' && ext.find_first_of(L"\\/") == std::wstring::npos;
}


/**
 * Checks if a `Content Type` value is one that `registerShell` writes.
 *
 * @param[in] ext - extension with leading dot
 * @param[in] value - value of `Content Type`
 * @return `true` if a decoder for the extension reports this MIME type
 */
static bool isOwnContentType(const std::wstring & ext, const std::wstring & value) {
	if ( value.empty() ) {
		return false;
	}
	const int count = FreeImage_GetFIFCount();
	for (int i = 0; i < count; ++i) {
		const FREE_IMAGE_FORMAT fif = FREE_IMAGE_FORMAT(i);
		if (( ! isDecoderFormat(fif) ) || toWide(FreeImage_GetFIFMimeType(fif)) != value) {
			continue;
		}
		if ( extensionMatches(fifExtensions(fif, L"", L';').c_str(), L"x" + ext) ) {
			return true;
		}
	}
	return false;
}


/**
 * Removes the shell integration written by `registerShell`.
 * Values are only removed if they still hold what `registerShell` has written,
 * so that settings changed or installed by others since remain untouched.
 *
 * @param[in] root - registry root of the registration
 */
static void unregisterShell(HKEY root) {
	const std::wstring shellKey = classesKey(std::wstring(progIdKey) + L"\\Shell");
	HKEY handle = nullptr;
	if (RegOpenKeyExW(root, shellKey.c_str(), 0, KEY_QUERY_VALUE, &handle) != ERROR_SUCCESS) {
		return;
	}
	std::vector<std::wstring> exts;
	for (DWORD index = 0; ; ++index) {
		wchar_t name[256];
		DWORD size = sizeof(name) / sizeof(name[0]);
		if (RegEnumValueW(handle, index, name, &size, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) {
			break;
		}
		exts.push_back(std::wstring(name, size));
	}
	RegCloseKey(handle);
	for (size_t i = 0; i < exts.size(); ++i) {
		const std::wstring & ext = exts[i];
		if ( ! isPlainExtension(ext) ) {
			continue;
		}
		const DWORD marks = readDword(root, shellKey, ext.c_str());
		if ((marks & FIWIC_MARK_THUMBNAIL) != 0) {
			const std::wstring handlerKey = classesKey(ext + L"\\" + thumbnailHandlerKey);
			if (readString(root, handlerKey, nullptr) == photoThumbnailProvider) {
				RegDeleteKeyW(root, handlerKey.c_str());
				deleteKeyIfEmpty(root, classesKey(ext + L"\\ShellEx"));
			}
		}
		if ((marks & FIWIC_MARK_PERCEIVED) != 0 && readString(root, classesKey(ext), L"PerceivedType") == L"image") {
			deleteValue(root, classesKey(ext), L"PerceivedType");
		}
		if ((marks & FIWIC_MARK_CONTENT) != 0 && isOwnContentType(ext, readString(root, classesKey(ext), L"Content Type"))) {
			deleteValue(root, classesKey(ext), L"Content Type");
		}
		if ((marks & FIWIC_MARK_KIND) != 0 && readString(HKEY_LOCAL_MACHINE, kindMapKey, ext.c_str(), KEY_WOW64_64KEY) == L"picture") {
			deleteValue(HKEY_LOCAL_MACHINE, kindMapKey, ext.c_str(), KEY_WOW64_64KEY);
		}
		if ((marks & FIWIC_MARK_OPENWITH) != 0) {
			deleteValue(root, classesKey(ext + L"\\OpenWithProgids"), photoViewerProgId);
			deleteKeyIfEmpty(root, classesKey(ext + L"\\OpenWithProgids"));
		}
		if ((marks & FIWIC_MARK_VIEWER) != 0 && readString(HKEY_LOCAL_MACHINE, photoViewerKey, ext.c_str(), KEY_WOW64_64KEY) == photoViewerProgId) {
			deleteValue(HKEY_LOCAL_MACHINE, photoViewerKey, ext.c_str(), KEY_WOW64_64KEY);
		}
		deleteKeyIfEmpty(root, classesKey(ext));
	}
}


/**
 * Removes the registration of this build below one registry root.
 * The ProgID and the shell integration go with the last registration of any
 * architecture only.
 *
 * @param[in] root - registry root
 * @return first error or `ERROR_SUCCESS`
 */
static LONG unregisterRoot(HKEY root) {
	LONG status = ERROR_SUCCESS;
	const int count = FreeImage_GetFIFCount();
	std::vector<std::wstring> keys;
	for (int i = 0; i < count; ++i) {
		const FREE_IMAGE_FORMAT fif = FREE_IMAGE_FORMAT(i);
		const std::wstring decoder = guidString(formatGuid(FIWIC_GUID_DECODER, fif));
		const std::wstring encoder = guidString(formatGuid(FIWIC_GUID_ENCODER, fif));
		keys.push_back(L"CLSID\\" + decoder);
		keys.push_back(L"CLSID\\" + encoder);
		keys.push_back(std::wstring(L"CLSID\\") + catIdDecoders + L"\\Instance\\" + decoder);
		keys.push_back(std::wstring(L"CLSID\\") + catIdEncoders + L"\\Instance\\" + encoder);
	}
	deleteValue(root, classesKey(viewsKey), viewName);
	if (countValues(root, classesKey(viewsKey)) == 0) {
		unregisterShell(root);
		keys.push_back(progIdKey);
	}
	for (size_t i = 0; i < keys.size(); ++i) {
		/* `SHDeleteKeyW` instead of `RegDeleteTreeW`, which Windows XP lacks. */
		const LONG res = LONG(SHDeleteKeyW(root, classesKey(keys[i]).c_str()));
		if (res != ERROR_SUCCESS && res != ERROR_FILE_NOT_FOUND && res != ERROR_PATH_NOT_FOUND) {
			status = res;
		}
	}
	/* Remove the category keys which a per user registration had to create. */
	const wchar_t * const categories[] = {catIdDecoders, catIdEncoders};
	for (size_t i = 0; i < (sizeof(categories) / sizeof(categories[0])); ++i) {
		deleteKeyIfEmpty(root, classesKey(std::wstring(L"CLSID\\") + categories[i] + L"\\Instance"));
		deleteKeyIfEmpty(root, classesKey(std::wstring(L"CLSID\\") + categories[i]));
	}
	deleteKeyIfEmpty(root, classesKey(L"CLSID"));
	return status;
}


HRESULT registerServer(bool perUser) {
	if ( ! ensureFreeImage() ) {
		return E_FAIL;
	}
	wchar_t path[MAX_PATH];
	const DWORD length = GetModuleFileNameW(moduleHandle(), path, MAX_PATH);
	if (length == 0 || length >= MAX_PATH) {
		return HRESULT_FROM_WIN32((length == 0) ? GetLastError() : ERROR_INSUFFICIENT_BUFFER);
	}
	try {
		const std::wstring module(path, length);
		/* Prefer a machine wide registration unless a per user one is requested. */
		HKEY root = perUser ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE;
		if ( ! perUser ) {
			HKEY probe = nullptr;
			const LONG access = RegCreateKeyExW(root, classesKey(progIdKey).c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &probe, nullptr);
			if (access == ERROR_SUCCESS) {
				RegCloseKey(probe);
			} else if (access == ERROR_ACCESS_DENIED) {
				root = HKEY_CURRENT_USER;
			} else {
				return HRESULT_FROM_WIN32(access);
			}
		}
		RegWriter reg(root);
		reg.string(classesKey(progIdKey), nullptr, FIWIC_FRIENDLY_NAME);
		std::vector<std::wstring> exts;
		std::vector<std::wstring> mimes;
		const int count = FreeImage_GetFIFCount();
		for (int i = 0; i < count; ++i) {
			const FREE_IMAGE_FORMAT fif = FREE_IMAGE_FORMAT(i);
			if ( isDecoderFormat(fif) ) {
				registerComponent(reg, module, fif, true);
				const std::wstring mime = toWide(FreeImage_GetFIFMimeType(fif));
				const std::wstring list = fifExtensions(fif, L".", L',');
				for (size_t pos = 0; pos < list.size(); ) {
					size_t end = list.find(L',', pos);
					if (end == std::wstring::npos) {
						end = list.size();
					}
					const std::wstring ext(list, pos, end - pos);
					bool known = false;
					for (size_t j = 0; j < exts.size(); ++j) {
						known = known || exts[j] == ext;
					}
					if ( ! known ) {
						exts.push_back(ext);
						mimes.push_back(mime);
					}
					pos = end + 1;
				}
			}
			if ( isEncoderFormat(fif) ) {
				registerComponent(reg, module, fif, false);
			}
		}
		reg.string(classesKey(viewsKey), viewName, module);
		if (reg.firstError() != ERROR_SUCCESS) {
			/* Roll back what this call may have written, but not the registrations below the other root. */
			const LONG status = reg.firstError();
			unregisterRoot(root);
			SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
			return HRESULT_FROM_WIN32(status);
		}
		for (size_t i = 0; i < exts.size(); ++i) {
			registerShell(root, exts[i], mimes[i]);
		}
		SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
		return S_OK;
	}
	FIWIC_CATCH_ALL
}


HRESULT unregisterServer() {
	if ( ! ensureFreeImage() ) {
		return E_FAIL;
	}
	try {
		LONG status = ERROR_SUCCESS;
		const HKEY roots[] = {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER};
		for (size_t r = 0; r < (sizeof(roots) / sizeof(roots[0])); ++r) {
			const LONG res = unregisterRoot(roots[r]);
			if (res != ERROR_SUCCESS) {
				status = res;
			}
		}
		SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
		return HRESULT_FROM_WIN32(status);
	}
	FIWIC_CATCH_ALL
}
