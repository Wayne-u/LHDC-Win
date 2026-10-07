#pragma once
#include <filesystem>
#include <string>
namespace lhdc {
void verify_package_signature(const std::filesystem::path& directory,const std::filesystem::path& certificate,const std::wstring& name=L"lhdc-transport");
}
