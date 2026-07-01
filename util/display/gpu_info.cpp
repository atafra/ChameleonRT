#include "display.h"

#ifdef _WIN32
#include <windows.h>
#include <setupapi.h>
#include <algorithm>
#include <cwctype>
#include <cwchar>
#include <sstream>
#include <vector>
#endif

namespace {

#ifdef _WIN32
const GUID CRT_GUID_DEVCLASS_DISPLAY = {
	0x4d36e968L, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};

std::string wide_to_utf8(const wchar_t *text)
{
	if (!text || text[0] == L'\0') {
		return std::string();
	}

	const int len = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
	if (len <= 1) {
		return std::string();
	}

	std::string result(static_cast<size_t>(len - 1), '\0');
	WideCharToMultiByte(CP_UTF8, 0, text, -1, &result[0], len, nullptr, nullptr);
	return result;
}

std::wstring pci_id_token(const char *prefix, uint32_t value)
{
	wchar_t buf[16];
	swprintf_s(buf, L"%S_%04X", prefix, value);
	return buf;
}

std::wstring uppercase(std::wstring value)
{
	std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) {
		return static_cast<wchar_t>(std::towupper(c));
	});
	return value;
}

bool device_matches_pci_id(HDEVINFO devices,
						   SP_DEVINFO_DATA &dev_info,
						   uint32_t vendor_id,
						   uint32_t device_id)
{
	DWORD required = 0;
	SetupDiGetDeviceRegistryPropertyW(devices,
									  &dev_info,
									  SPDRP_HARDWAREID,
									  nullptr,
									  nullptr,
									  0,
									  &required);
	if (required == 0) {
		return false;
	}

	std::vector<BYTE> buffer(required);
	if (!SetupDiGetDeviceRegistryPropertyW(devices,
										   &dev_info,
										   SPDRP_HARDWAREID,
										   nullptr,
										   buffer.data(),
										   static_cast<DWORD>(buffer.size()),
										   nullptr)) {
		return false;
	}

	const std::wstring ven = pci_id_token("VEN", vendor_id);
	const std::wstring dev = pci_id_token("DEV", device_id);
	const wchar_t *entry = reinterpret_cast<const wchar_t *>(buffer.data());
	while (entry && *entry) {
		const std::wstring hardware_id = uppercase(entry);
		if (hardware_id.find(ven) != std::wstring::npos &&
			hardware_id.find(dev) != std::wstring::npos) {
			return true;
		}
		entry += std::wcslen(entry) + 1;
	}
	return false;
}

std::string registry_property_string(HDEVINFO devices, SP_DEVINFO_DATA &dev_info, DWORD property)
{
	DWORD required = 0;
	SetupDiGetDeviceRegistryPropertyW(devices,
									  &dev_info,
									  property,
									  nullptr,
									  nullptr,
									  0,
									  &required);
	if (required == 0) {
		return std::string();
	}

	std::vector<BYTE> buffer(required + sizeof(wchar_t));
	if (!SetupDiGetDeviceRegistryPropertyW(devices,
										   &dev_info,
										   property,
										   nullptr,
										   buffer.data(),
										   static_cast<DWORD>(buffer.size()),
										   nullptr)) {
		return std::string();
	}

	return wide_to_utf8(reinterpret_cast<const wchar_t *>(buffer.data()));
}

std::string driver_version_from_key(const std::string &driver_key)
{
	if (driver_key.empty()) {
		return std::string();
	}

	const std::wstring key_path = L"SYSTEM\\CurrentControlSet\\Control\\Class\\" +
		std::wstring(driver_key.begin(), driver_key.end());

	HKEY key = nullptr;
	if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key_path.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) {
		return std::string();
	}

	DWORD type = 0;
	DWORD bytes = 0;
	if (RegQueryValueExW(key, L"DriverVersion", nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
		type != REG_SZ || bytes == 0) {
		RegCloseKey(key);
		return std::string();
	}

	std::vector<wchar_t> value((bytes / sizeof(wchar_t)) + 1, L'\0');
	if (RegQueryValueExW(key,
						 L"DriverVersion",
						 nullptr,
						 nullptr,
						 reinterpret_cast<LPBYTE>(value.data()),
						 &bytes) != ERROR_SUCCESS) {
		RegCloseKey(key);
		return std::string();
	}

	RegCloseKey(key);
	return wide_to_utf8(value.data());
}
#endif

}  // namespace

GpuInfo make_gpu_info(const std::string &driver, uint32_t vendor_id, uint32_t device_id)
{
	GpuInfo info{driver, driver, std::string()};

#ifdef _WIN32
	if (vendor_id == 0 || device_id == 0) {
		return info;
	}

	HDEVINFO devices = SetupDiGetClassDevsW(&CRT_GUID_DEVCLASS_DISPLAY,
											nullptr,
											nullptr,
											DIGCF_PRESENT);
	if (devices == INVALID_HANDLE_VALUE) {
		return info;
	}

	SP_DEVINFO_DATA dev_info = {};
	dev_info.cbSize = sizeof(dev_info);
	for (DWORD i = 0; SetupDiEnumDeviceInfo(devices, i, &dev_info); ++i) {
		if (!device_matches_pci_id(devices, dev_info, vendor_id, device_id)) {
			continue;
		}

		std::string name = registry_property_string(devices, dev_info, SPDRP_FRIENDLYNAME);
		if (name.empty()) {
			name = registry_property_string(devices, dev_info, SPDRP_DEVICEDESC);
		}
		if (!name.empty()) {
			info.name = name;
		}

		const std::string driver_key = registry_property_string(devices, dev_info, SPDRP_DRIVER);
		info.driver_version = driver_version_from_key(driver_key);
		break;
	}

	SetupDiDestroyDeviceInfoList(devices);
#endif

	return info;
}
