/**
 * @file fiwic.cpp
 * @author Daniel Starke
 * @date 2024-12-03
 * @version 2026-10-26
 *
 * Entry points of fiwic.dll: class factory and COM server exports.
 */
#include <wchar.h>
#include <windows.h>
#include <objbase.h>
#include <wincodec.h>
#include <FreeImage.h>
#include <fiwicutil.hpp>


/**
 * Class factory of the decoder or encoder of one FreeImage format.
 */
class ClassFactory : public ComObject<IClassFactory> {
private:
	bool decoder; /**< `true` to create decoders, `false` for encoders. */
	FREE_IMAGE_FORMAT fif; /**< FreeImage format of the created components. */

public:
	/**
	 * Constructor.
	 *
	 * @param[in] aDecoder - `true` to create decoders, `false` for encoders
	 * @param[in] aFif - FreeImage format of the created components
	 */
	ClassFactory(bool aDecoder, FREE_IMAGE_FORMAT aFif):
		ComObject<IClassFactory>(),
		decoder(aDecoder),
		fif(aFif)
	{}

	/** Destructor. */
	~ClassFactory() noexcept override = default;

	STDMETHOD(CreateInstance)(IUnknown * pUnkOuter, REFIID riid, void ** ppvObject) noexcept override {
		if (ppvObject == nullptr) {
			return E_POINTER;
		}
		*ppvObject = nullptr;
		if (pUnkOuter != nullptr) {
			return CLASS_E_NOAGGREGATION;
		}
		return this->decoder ? createDecoder(this->fif, riid, ppvObject) : createEncoder(this->fif, riid, ppvObject);
	}

	STDMETHOD(LockServer)(BOOL fLock) noexcept override {
		lockServer(fLock != FALSE);
		return S_OK;
	}
};


extern "C" BOOL WINAPI DllMain(HINSTANCE hInstance, DWORD reason, LPVOID reserved) {
	return dllMain(hInstance, reason, reserved);
}


extern "C" HRESULT STDAPICALLTYPE DllCanUnloadNow() {
	return (objectCount() == 0 && lockCount() == 0) ? S_OK : S_FALSE;
}


extern "C" HRESULT STDAPICALLTYPE DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID * ppv) {
	if (ppv == nullptr) {
		return E_POINTER;
	}
	*ppv = nullptr;
	ensureFreeImage();
	bool decoder = true;
	FREE_IMAGE_FORMAT fif = FIF_UNKNOWN;
	if (parseFormatGuid(FIWIC_GUID_DECODER, rclsid, &fif) && isDecoderFormat(fif)) {
		decoder = true;
	} else if (parseFormatGuid(FIWIC_GUID_ENCODER, rclsid, &fif) && isEncoderFormat(fif)) {
		decoder = false;
	} else {
		return CLASS_E_CLASSNOTAVAILABLE;
	}
	ClassFactory * factory = new(std::nothrow) ClassFactory(decoder, fif);
	if (factory == nullptr) {
		return E_OUTOFMEMORY;
	}
	const HRESULT hr = factory->QueryInterface(riid, ppv);
	factory->Release();
	return hr;
}


extern "C" HRESULT STDAPICALLTYPE DllRegisterServer() {
	return registerServer(false);
}


extern "C" HRESULT STDAPICALLTYPE DllUnregisterServer() {
	return unregisterServer();
}


extern "C" HRESULT STDAPICALLTYPE DllInstall(BOOL bInstall, PCWSTR pszCmdLine) {
	if (bInstall == FALSE) {
		return unregisterServer();
	}
	/* regsvr32 /n /i:user fiwic.dll */
	return registerServer(pszCmdLine != nullptr && _wcsicmp(pszCmdLine, L"user") == 0);
}
