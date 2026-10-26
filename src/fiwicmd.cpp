/**
 * @file fiwicmd.cpp
 * @author Daniel Starke
 * @date 2024-12-03
 * @version 2026-10-26
 *
 * WIC metadata query reader exposing the FreeImage metadata models.
 *
 * Each readable FreeImage metadata model is published as a WIC metadata group
 * named `freeimage!<MODEL>`. The group members are the FreeImage tags of that
 * model rendered through `FreeImage_TagToString`.
 */
#include <string.h>
#include <wchar.h>
#include <windows.h>
#include <objbase.h>
#include <wincodec.h>
#include <oleidl.h>
#include <propidl.h>
#include <unknwn.h>
#include <string>
#include <vector>
#include <FreeImage.h>
#include <fiwicutil.hpp>


/** Mapping of a FreeImage metadata model to its WIC group name. */
struct ModelEntry {
	FREE_IMAGE_MDMODEL model; /**< FreeImage metadata model. */
	const wchar_t * name; /**< Group name suffix reported to WIC. */
};


/** Readable FreeImage metadata models. */
static const ModelEntry models[] = {
	{FIMD_COMMENTS, L"COMMENTS"},
	{FIMD_EXIF_MAIN, L"EXIF_MAIN"},
	{FIMD_EXIF_EXIF, L"EXIF"},
	{FIMD_EXIF_GPS, L"GPS"},
	{FIMD_EXIF_MAKERNOTE, L"MAKERNOTE"},
	{FIMD_EXIF_INTEROP, L"INTEROP"},
	{FIMD_IPTC, L"IPTC"},
	{FIMD_XMP, L"XMP"},
	{FIMD_GEOTIFF, L"GEOTIFF"},
	{FIMD_ANIMATION, L"ANIMATION"},
	{FIMD_CUSTOM, L"CUSTOM"},
	{FIMD_EXIF_RAW, L"EXIF_RAW"}
};


/** Lower case vendor prefix of every exposed group. */
static const wchar_t mdVendor[] = L"freeimage";


/**
 * Serializes the FreeImage metadata calls. `FreeImage_TagToString` renders into
 * a buffer shared by the whole process and metadata iteration of a bitmap is not
 * thread safe either.
 */
static CriticalSection gMetadataLock;


/** One metadata item of a group. */
struct MetadataItem {
	std::wstring name; /**< Tag key. */
	std::wstring value; /**< Tag value in string form. */

	/**
	 * Constructor.
	 *
	 * @param[in] n - tag key
	 * @param[in] v - tag value
	 */
	MetadataItem(const std::wstring & n, const std::wstring & v):
		name(n),
		value(v)
	{}
};


/** One metadata group. */
struct MetadataGroup {
	std::wstring name; /**< Group name, e.g. `freeimage!EXIF`. */
	std::vector<MetadataItem> items; /**< Members of the group. */
};


/**
 * Simple `IEnumString` over a string vector.
 */
class EnumString : public ComObject<IEnumString> {
private:
	std::vector<std::wstring> items; /**< Enumerated strings. */
	size_t pos; /**< Current position. */
public:
	/**
	 * Constructor.
	 *
	 * @param[in] aItems - strings to enumerate
	 */
	explicit EnumString(const std::vector<std::wstring> & aItems):
		ComObject<IEnumString>(),
		items(aItems),
		pos(0)
	{}

	/** Destructor. */
	~EnumString() noexcept override = default;

	STDMETHOD(Next)(ULONG celt, LPWSTR * rgelt, ULONG * pceltFetched) noexcept override {
		if (rgelt == nullptr) {
			return E_POINTER;
		}
		if (celt > 1 && pceltFetched == nullptr) {
			return E_INVALIDARG;
		}
		ULONG n = 0;
		while (n < celt && this->pos < this->items.size()) {
			const size_t len = this->items[this->pos].size() + 1;
			LPWSTR str = LPWSTR(CoTaskMemAlloc(len * sizeof(wchar_t)));
			if (str == nullptr) {
				/* The caller does not free the strings if the call fails. */
				while (n > 0) {
					--n;
					CoTaskMemFree(rgelt[n]);
					rgelt[n] = nullptr;
					--(this->pos);
				}
				if (pceltFetched != nullptr) {
					*pceltFetched = 0;
				}
				return E_OUTOFMEMORY;
			}
			wmemcpy(str, this->items[this->pos].c_str(), len);
			rgelt[n] = str;
			++n;
			++(this->pos);
		}
		if (pceltFetched != nullptr) {
			*pceltFetched = n;
		}
		return (n == celt) ? S_OK : S_FALSE;
	}

	STDMETHOD(Skip)(ULONG celt) noexcept override {
		if (celt > (this->items.size() - this->pos)) {
			this->pos = this->items.size();
			return S_FALSE;
		}
		this->pos += celt;
		return S_OK;
	}

	STDMETHOD(Reset)() noexcept override {
		this->pos = 0;
		return S_OK;
	}

	STDMETHOD(Clone)(IEnumString ** out) noexcept override {
		if (out == nullptr) {
			return E_POINTER;
		}
		*out = nullptr;
		try {
			EnumString * e = new(std::nothrow) EnumString(this->items);
			if (e == nullptr) {
				return E_OUTOFMEMORY;
			}
			e->pos = this->pos;
			*out = e;
			return S_OK;
		} FIWIC_CATCH_ALL
	}
};


/**
 * `IWICMetadataQueryReader` over a snapshot of a FreeImage bitmap's metadata.
 *
 * The interface published by the Win7 era `wincodec.h` is implemented, which
 * is the contract WIC uses for third party codecs on every Windows version.
 */
class MetadataQueryReader : public ComObject<IWICMetadataQueryReader> {
private:
	GUID container; /**< Container format reported by `GetContainerFormat`. */
	std::wstring location; /**< Namespace reported by `GetLocation`. */
	std::vector<MetadataGroup> groups; /**< snapshot of the metadata. */
public:
	/**
	 * Constructor.
	 *
	 * @param[in] dib - bitmap to read metadata from
	 * @param[in] aContainer - container format of the image
	 * @param[in] top - `true` for the top level reader
	 */
	MetadataQueryReader(FIBITMAP * dib, const GUID & aContainer, bool top):
		ComObject<IWICMetadataQueryReader>(),
		container(aContainer),
		location(),
		groups()
	{
		this->snapshot(dib, top);
		if ( top ) {
			this->location = L"/";
			this->location += mdVendor;
			this->location += L'!';
		} else {
			this->location = L"/";
		}
	}

	/** Destructor. */
	~MetadataQueryReader() noexcept override = default;

	STDMETHOD(GetContainerFormat)(GUID * pguidContainerFormat) noexcept override {
		if (pguidContainerFormat == nullptr) {
			return E_POINTER;
		}
		*pguidContainerFormat = this->container;
		return S_OK;
	}

	STDMETHOD(GetLocation)(UINT cchMaxLength, WCHAR * wzNamespace, UINT * pcchActualLength) noexcept override {
		if (pcchActualLength == nullptr) {
			return E_POINTER;
		}
		const UINT need = UINT(this->location.size()) + 1;
		*pcchActualLength = need;
		if (wzNamespace == nullptr) {
			/* Query of the required length. */
			return (cchMaxLength == 0) ? S_OK : E_INVALIDARG;
		}
		if (cchMaxLength < need) {
			return WINCODEC_ERR_INSUFFICIENTBUFFER;
		}
		wmemcpy(wzNamespace, this->location.c_str(), need);
		return S_OK;
	}

	STDMETHOD(GetMetadataByName)(LPCWSTR wzName, PROPVARIANT * pvarValue) noexcept override {
		/* The value is an output only parameter. It is optional to check for existence. */
		if (pvarValue != nullptr) {
			PropVariantInit(pvarValue);
		}
		if (wzName == nullptr) {
			return E_POINTER;
		}
		try {
			std::wstring path(wzName);
			if (( ! path.empty() ) && path[0] == L'/') {
				path.erase(0, 1);
			}
			if ( path.empty() ) {
				return WINCODEC_ERR_INVALIDQUERYCHARACTER;
			}
			/* Split into group and member. A bare /Name addresses the flat view. */
			std::wstring groupName;
			std::wstring member;
			const size_t slash = path.find(L'/');
			if (slash == std::wstring::npos) {
				member = path;
			} else {
				groupName = std::wstring(path, 0, slash);
				member = std::wstring(path, slash + 1, path.size() - slash - 1);
			}
			if ( member.empty() ) {
				return WINCODEC_ERR_INVALIDQUERYCHARACTER;
			}
			for (size_t g = 0; g < this->groups.size(); ++g) {
				if (( ! groupName.empty() ) && this->groups[g].name != groupName) {
					continue;
				}
				for (size_t i = 0; i < this->groups[g].items.size(); ++i) {
					if (this->groups[g].items[i].name != member) {
						continue;
					}
					if (pvarValue == nullptr) {
						return S_OK;
					}
					const size_t len = this->groups[g].items[i].value.size() + 1;
					LPWSTR str = LPWSTR(CoTaskMemAlloc(len * sizeof(wchar_t)));
					if (str == nullptr) {
						return E_OUTOFMEMORY;
					}
					wmemcpy(str, this->groups[g].items[i].value.c_str(), len);
					pvarValue->vt = VT_LPWSTR;
					pvarValue->pwszVal = str;
					return S_OK;
				}
			}
			return WINCODEC_ERR_PROPERTYNOTFOUND;
		} FIWIC_CATCH_ALL
	}

	STDMETHOD(GetEnumerator)(IEnumString ** ppIEnumString) noexcept override {
		if (ppIEnumString == nullptr) {
			return E_POINTER;
		}
		*ppIEnumString = nullptr;
		try {
			std::vector<std::wstring> names;
			for (size_t g = 0; g < this->groups.size(); ++g) {
				for (size_t i = 0; i < this->groups[g].items.size(); ++i) {
					names.push_back(this->groups[g].items[i].name);
				}
			}
			EnumString * e = new(std::nothrow) EnumString(names);
			if (e == nullptr) {
				return E_OUTOFMEMORY;
			}
			*ppIEnumString = e;
			return S_OK;
		} FIWIC_CATCH_ALL
	}
private:
	/**
	 * Collects the metadata of a bitmap into groups.
	 *
	 * @param[in] dib - bitmap to inspect
	 * @param[in] top - `true` to expose the vendor groups, `false` for the
	 * flat frame scoped form
	 */
	void snapshot(FIBITMAP * dib, bool top) {
		this->groups.clear();
		if (dib == nullptr) {
			return;
		}
		Guard guard(gMetadataLock);
		for (size_t i = 0; i < (sizeof(models) / sizeof(models[0])); ++i) {
			const FREE_IMAGE_MDMODEL model = models[i].model;
			FITAG * tag = nullptr;
			FIMETADATA * find = FreeImage_FindFirstMetadata(model, dib, &tag);
			if (find == nullptr) {
				continue;
			}
			MetadataGroup group;
			if ( top ) {
				group.name = mdVendor;
				group.name += L'!';
				group.name += models[i].name;
			}
			for (bool more = true; more && tag != nullptr; ) {
				const char * key = FreeImage_GetTagKey(tag);
				const char * str = FreeImage_TagToString(model, tag, nullptr);
				const std::wstring name = toWide(key);
				if ( ! name.empty() ) {
					group.items.push_back(MetadataItem(name, toWide(str)));
				}
				more = FreeImage_FindNextMetadata(find, &tag) ? true : false;
			}
			FreeImage_FindCloseMetadata(find);
			if ( ! group.items.empty() ) {
				this->groups.push_back(group);
			}
		}
	}

	/**
	 * Finds the index of a group by name.
	 *
	 * @param[in] name - group name (case sensitive)
	 * @param[out] index - index of the group
	 * @return `true` when the group exists
	 */
	bool findGroup(const std::wstring & name, size_t * index) const {
		for (size_t i = 0; i < this->groups.size(); ++i) {
			if (this->groups[i].name == name) {
				*index = i;
				return true;
			}
		}
		return false;
	}
};


/**
 * Creates a metadata query reader for a FreeImage bitmap.
 *
 * @param[in] dib - bitmap to read metadata from
 * @param[in] container - container format reported by the reader
 * @param[in] top - `true` for the top level reader
 * @param[out] out - created reader
 * @return result code
 */
HRESULT createMetadataQueryReader(FIBITMAP * dib, const GUID & container, bool top, IWICMetadataQueryReader ** out) {
	if (out == nullptr) {
		return E_POINTER;
	}
	*out = nullptr;
	try {
		MetadataQueryReader * reader = new(std::nothrow) MetadataQueryReader(dib, container, top);
		if (reader == nullptr) {
			return E_OUTOFMEMORY;
		}
		*out = reader;
		return S_OK;
	} FIWIC_CATCH_ALL
}
