#pragma once

#if defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>

#include <cstdint>
#include <vector>

#pragma comment(lib, "advapi32.lib")

namespace geocoin::utils {

class ScopedWindowsHandle {
public:
    explicit ScopedWindowsHandle(HANDLE handle = nullptr) noexcept
        : handle_(handle) {}

    ~ScopedWindowsHandle() {
        reset();
    }

    ScopedWindowsHandle(const ScopedWindowsHandle&) = delete;
    ScopedWindowsHandle& operator=(const ScopedWindowsHandle&) = delete;

    HANDLE get() const noexcept { return handle_; }

    void reset(HANDLE handle = nullptr) noexcept {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            (void)::CloseHandle(handle_);
        }
        handle_ = handle;
    }

private:
    HANDLE handle_ = nullptr;
};

class CurrentUserOnlySecurity {
public:
    CurrentUserOnlySecurity() noexcept {
        try {
            initialize();
        } catch (...) {
            reset();
        }
    }

    ~CurrentUserOnlySecurity() {
        reset();
    }

    CurrentUserOnlySecurity(const CurrentUserOnlySecurity&) = delete;
    CurrentUserOnlySecurity& operator=(const CurrentUserOnlySecurity&) = delete;

    bool valid() const noexcept { return valid_; }
    SECURITY_ATTRIBUTES* attributes() noexcept { return valid_ ? &attributes_ : nullptr; }

private:
    void initialize() {
        HANDLE raw_token = nullptr;
        if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &raw_token)) {
            return;
        }
        ScopedWindowsHandle token(raw_token);

        DWORD required = 0;
        (void)::GetTokenInformation(token.get(), TokenUser, nullptr, 0, &required);
        token_user_.resize(required);
        if (required == 0 ||
            !::GetTokenInformation(token.get(), TokenUser, token_user_.data(), required, &required)) {
            token_user_.clear();
            return;
        }

        auto* token_user = reinterpret_cast<TOKEN_USER*>(token_user_.data());
        EXPLICIT_ACCESSW access{};
        access.grfAccessPermissions = FILE_ALL_ACCESS;
        access.grfAccessMode = SET_ACCESS;
        access.grfInheritance = NO_INHERITANCE;
        access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        access.Trustee.TrusteeType = TRUSTEE_IS_USER;
        access.Trustee.ptstrName = static_cast<LPWSTR>(token_user->User.Sid);
        if (::SetEntriesInAclW(1, &access, nullptr, &acl_) != ERROR_SUCCESS) {
            acl_ = nullptr;
            return;
        }

        if (!::InitializeSecurityDescriptor(&descriptor_, SECURITY_DESCRIPTOR_REVISION) ||
            !::SetSecurityDescriptorDacl(&descriptor_, TRUE, acl_, FALSE) ||
            !::SetSecurityDescriptorControl(
                &descriptor_,
                SE_DACL_PROTECTED,
                SE_DACL_PROTECTED
            )) {
            reset();
            return;
        }

        attributes_.nLength = sizeof(attributes_);
        attributes_.lpSecurityDescriptor = &descriptor_;
        attributes_.bInheritHandle = FALSE;
        valid_ = true;
    }

    void reset() noexcept {
        if (acl_ != nullptr) {
            (void)::LocalFree(acl_);
            acl_ = nullptr;
        }
        valid_ = false;
    }

    std::vector<unsigned char> token_user_;
    PACL acl_ = nullptr;
    SECURITY_DESCRIPTOR descriptor_{};
    SECURITY_ATTRIBUTES attributes_{};
    bool valid_ = false;
};

inline bool is_regular_nonreparse_file(HANDLE handle, uint64_t* size = nullptr) noexcept {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!::GetFileInformationByHandle(handle, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
        return false;
    }
    if (size != nullptr) {
        *size = (static_cast<uint64_t>(info.nFileSizeHigh) << 32) |
            static_cast<uint64_t>(info.nFileSizeLow);
    }
    return true;
}

inline bool restrict_file_to_current_user(HANDLE handle) noexcept {
    try {
        CurrentUserOnlySecurity security;
        if (!security.valid()) {
            return false;
        }
        auto* descriptor = static_cast<SECURITY_DESCRIPTOR*>(
            security.attributes()->lpSecurityDescriptor
        );
        BOOL present = FALSE;
        BOOL defaulted = FALSE;
        PACL acl = nullptr;
        if (!::GetSecurityDescriptorDacl(descriptor, &present, &acl, &defaulted) ||
            !present || acl == nullptr) {
            return false;
        }
        return ::SetSecurityInfo(
            handle,
            SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
            nullptr,
            nullptr,
            acl,
            nullptr
        ) == ERROR_SUCCESS;
    } catch (...) {
        return false;
    }
}

} // namespace geocoin::utils

#endif
