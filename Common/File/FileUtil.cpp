// Copyright (C) 2003 Dolphin Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official SVN repository and contact information can be found at
// http://code.google.com/p/dolphin-emu/

#if defined(_MSC_VER)
#pragma warning(disable:4091)  // workaround bug in VS2015 headers
#ifndef UNICODE
#error Win32 build requires a unicode build
#endif
#endif

#include "ppsspp_config.h"

#ifdef __MINGW32__
#include <unistd.h>
#ifndef _POSIX_THREAD_SAFE_FUNCTIONS
#define _POSIX_THREAD_SAFE_FUNCTIONS 200112L
#endif
#endif
#include <cstring>
#include <ctime>
#include <memory>

#include "Common/Log.h"
#include "Common/File/FileUtil.h"
#include "Common/StringUtils.h"
#include "Common/SysError.h"

#ifdef _WIN32
#include "Common/CommonWindows.h"
#include <Windows.h>
#include <shlobj.h>		// for SHGetFolderPath
#include <shellapi.h>
#include <commdlg.h>	// for GetSaveFileName
#include <io.h>
#include <direct.h>		// getcwd
#else
#include <sys/param.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#endif

#if defined(__DragonFly__) || defined(__FreeBSD__) || defined(__FreeBSD_kernel__) || defined(__NetBSD__)
#include <sys/sysctl.h>		// KERN_PROC_PATHNAME
#endif

#if defined(__APPLE__)
#include <CoreFoundation/CFString.h>
#include <CoreFoundation/CFURL.h>
#include <CoreFoundation/CFBundle.h>
#if !PPSSPP_PLATFORM(IOS)
#include <mach-o/dyld.h>
#endif  // !PPSSPP_PLATFORM(IOS)
#endif  // __APPLE__

#include "Common/Data/Encoding/Utf8.h"

#include <sys/stat.h>

#ifndef S_ISDIR
#define S_ISDIR(m)  (((m)&S_IFMT) == S_IFDIR)
#endif

#if !defined(__linux__) && !defined(_WIN32) && !defined(__QNX__)
#define stat64 stat
#define fstat64 fstat
#endif

#define DIR_SEP "/"
#ifdef _WIN32
#define DIR_SEP_CHRS "/\\"
#else
#define DIR_SEP_CHRS "/"
#endif

// This namespace has various generic functions related to files and paths.
// The code still needs a ton of cleanup.
// REMEMBER: strdup considered harmful!
namespace File {

FILE *OpenCFile(const Path &path, const char *mode) {
	switch (path.Type()) {
	case PathType::NATIVE:
		break;
	default:
		ERROR_LOG(COMMON, "OpenCFile(%s): Not yet supported", path.c_str());
		return nullptr;
	}

#if defined(_WIN32) && defined(UNICODE)
	return _wfopen(path.ToWString().c_str(), ConvertUTF8ToWString(mode).c_str());
#else
	return fopen(path.ToString().c_str(), mode);
#endif
}

#ifdef _WIN32
static bool ResolvePathVista(const std::wstring &path, wchar_t *buf, DWORD bufSize) {
	typedef DWORD(WINAPI *getFinalPathNameByHandleW_f)(HANDLE hFile, LPWSTR lpszFilePath, DWORD cchFilePath, DWORD dwFlags);
	static getFinalPathNameByHandleW_f getFinalPathNameByHandleW = nullptr;

#if PPSSPP_PLATFORM(UWP)
	getFinalPathNameByHandleW = &GetFinalPathNameByHandleW;
#else
	if (!getFinalPathNameByHandleW) {
		HMODULE kernel32 = GetModuleHandle(L"kernel32.dll");
		getFinalPathNameByHandleW = (getFinalPathNameByHandleW_f)GetProcAddress(kernel32, "GetFinalPathNameByHandleW");
	}
#endif

	if (getFinalPathNameByHandleW) {
#if PPSSPP_PLATFORM(UWP)
		HANDLE hFile = CreateFile2(path.c_str(), GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, nullptr);
#else
		HANDLE hFile = CreateFile(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
#endif
		if (hFile == INVALID_HANDLE_VALUE)
			return false;

		DWORD result = getFinalPathNameByHandleW(hFile, buf, bufSize - 1, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
		CloseHandle(hFile);

		return result < bufSize && result != 0;
	}

	return false;
}
#endif

std::string ResolvePath(const std::string &path) {
	if (startsWith(path, "http://") || startsWith(path, "https://")) {
		return path;
	}
#if PPSSPP_PLATFORM(WINDOWS)
	static const int BUF_SIZE = 32768;
	wchar_t *buf = new wchar_t[BUF_SIZE] {};

	std::wstring input = ConvertUTF8ToWString(path);
	// Try to resolve symlinks (such as Documents aliases, etc.) if possible on Vista and higher.
	// For some paths and remote shares, this may fail, so fall back.
	if (!ResolvePathVista(input, buf, BUF_SIZE)) {
		wchar_t *longBuf = new wchar_t[BUF_SIZE] {};

		int result = GetLongPathNameW(input.c_str(), longBuf, BUF_SIZE - 1);
		if (result >= BUF_SIZE || result == 0)
			wcscpy_s(longBuf, BUF_SIZE - 1, input.c_str());

		result = GetFullPathNameW(longBuf, BUF_SIZE - 1, buf, nullptr);
		if (result >= BUF_SIZE || result == 0)
			wcscpy_s(buf, BUF_SIZE - 1, input.c_str());

		delete [] longBuf;
	}

	// Normalize slashes just in case.
	for (int i = 0; i < BUF_SIZE; ++i) {
		if (buf[i] == '\\')
			buf[i] = '/';
		else if (buf[i] == '\0')
			break;
	}

	// Undo the \\?\C:\ syntax that's normally returned (after normalization of slashes.)
	std::string output = ConvertWStringToUTF8(buf);
	if (buf[0] == '/' && buf[1] == '/' && buf[2] == '?' && buf[3] == '/' && isalpha(buf[4]) && buf[5] == ':')
		output = output.substr(4);
	delete [] buf;
	return output;

#else
	std::unique_ptr<char[]> buf(new char[PATH_MAX + 32768]);
	if (realpath(path.c_str(), buf.get()) == nullptr)
		return path;
	return buf.get();
#endif
}

// Remove any ending forward slashes from directory paths
// Modifies argument.
static void StripTailDirSlashes(std::string &fname) {
	if (fname.length() > 1) {
		size_t i = fname.length() - 1;
#if PPSSPP_PLATFORM(WINDOWS)
		if (i == 2 && fname[1] == ':' && fname[2] == '\\')
			return;
#endif
		while (strchr(DIR_SEP_CHRS, fname[i]))
			fname[i--] = '\0';
	}
	return;
}

// Returns true if file filename exists. Will return true on directories.
bool ExistsInDir(const Path &path, const std::string &filename) {
	return Exists(path / filename);
}

bool Exists(const std::string &filename) {
	std::string fn = filename;
	StripTailDirSlashes(fn);

#if defined(_WIN32)
	std::wstring copy = ConvertUTF8ToWString(fn);

	// Make sure Windows will no longer handle critical errors, which means no annoying "No disk" dialog
#if !PPSSPP_PLATFORM(UWP)
	int OldMode = SetErrorMode(SEM_FAILCRITICALERRORS);
#endif
	WIN32_FILE_ATTRIBUTE_DATA data{};
	if (!GetFileAttributesEx(copy.c_str(), GetFileExInfoStandard, &data) || data.dwFileAttributes == INVALID_FILE_ATTRIBUTES) {
		return false;
	}
#if !PPSSPP_PLATFORM(UWP)
	SetErrorMode(OldMode);
#endif
	return true;
#else
	struct stat file_info;
	return stat(fn.c_str(), &file_info) == 0;
#endif
}

bool Exists(const Path &path) {
	// Temporary forward.
	return Exists(path.ToString());
}

// Returns true if filename exists and is a directory
bool IsDirectory(const Path &filename) {
	switch (filename.Type()) {
	case PathType::NATIVE:
		break; // OK
	default:
		return false;
	}

	std::string fn = filename.ToString();
	StripTailDirSlashes(fn);

#if defined(_WIN32)
	std::wstring copy = ConvertUTF8ToWString(fn);
	WIN32_FILE_ATTRIBUTE_DATA data{};
	if (!GetFileAttributesEx(copy.c_str(), GetFileExInfoStandard, &data) || data.dwFileAttributes == INVALID_FILE_ATTRIBUTES) {
		auto err = GetLastError();
		WARN_LOG(COMMON, "GetFileAttributes failed on %s: %08x %s", fn.c_str(), (uint32_t)err, GetStringErrorMsg(err).c_str());
		return false;
	}
	DWORD result = data.dwFileAttributes;
	return (result & FILE_ATTRIBUTE_DIRECTORY) == FILE_ATTRIBUTE_DIRECTORY;
#else
	std::string copy(fn);
	struct stat file_info;
	int result = stat(copy.c_str(), &file_info);
	if (result < 0) {
		WARN_LOG(COMMON, "IsDirectory: stat failed on %s: %s", fn.c_str(), GetLastErrorMsg().c_str());
		return false;
	}
	return S_ISDIR(file_info.st_mode);
#endif
}

// Deletes a given filename, return true on success
// Doesn't supports deleting a directory
bool Delete(const Path &filename) {
	switch (filename.Type()) {
	case PathType::NATIVE:
		break; // OK
	default:
		return false;
	}

	INFO_LOG(COMMON, "Delete: file %s", filename.c_str());

	// Return true because we care about the file no 
	// being there, not the actual delete.
	if (!Exists(filename)) {
		WARN_LOG(COMMON, "Delete: %s does not exists", filename.c_str());
		return true;
	}

	// We can't delete a directory
	if (IsDirectory(filename)) {
		WARN_LOG(COMMON, "Delete failed: %s is a directory", filename.c_str());
		return false;
	}

#ifdef _WIN32
	if (!DeleteFile(filename.ToWString().c_str())) {
		WARN_LOG(COMMON, "Delete: DeleteFile failed on %s: %s", filename.c_str(), GetLastErrorMsg().c_str());
		return false;
	}
#else
	if (unlink(filename.c_str()) == -1) {
		WARN_LOG(COMMON, "Delete: unlink failed on %s: %s", 
				 filename.c_str(), GetLastErrorMsg().c_str());
		return false;
	}
#endif

	return true;
}

// Returns true if successful, or path already exists.
bool CreateDir(const Path &path) {
	switch (path.Type()) {
	case PathType::NATIVE:
		break; // OK
	default:
		return false;
	}

	std::string fn = path.ToString();
	DEBUG_LOG(COMMON, "CreateDir('%s')", fn.c_str());
#ifdef _WIN32
	if (::CreateDirectory(ConvertUTF8ToWString(fn).c_str(), NULL))
		return true;
	DWORD error = GetLastError();
	if (error == ERROR_ALREADY_EXISTS)
	{
		WARN_LOG(COMMON, "CreateDir: CreateDirectory failed on %s: already exists", path.c_str());
		return true;
	}
	ERROR_LOG(COMMON, "CreateDir: CreateDirectory failed on %s: %08x %s", path.c_str(), (uint32_t)error, GetStringErrorMsg(error).c_str());
	return false;
#else
	if (mkdir(fn.c_str(), 0755) == 0)
		return true;

	int err = errno;
	if (err == EEXIST)
	{
		WARN_LOG(COMMON, "CreateDir: mkdir failed on %s: already exists", fn.c_str());
		return true;
	}

	ERROR_LOG(COMMON, "CreateDir: mkdir failed on %s: %s", fn.c_str(), strerror(err));
	return false;
#endif
}

// Creates the full path of fullPath returns true on success
bool CreateFullPath(const Path &path) {
	switch (path.Type()) {
	case PathType::NATIVE:
		break; // OK
	default:
		return false;
	}

	std::string fullPath = path.ToString();
	int panicCounter = 100;
	VERBOSE_LOG(COMMON, "CreateFullPath: path %s", fullPath.c_str());
		
	if (File::Exists(fullPath)) {
		DEBUG_LOG(COMMON, "CreateFullPath: path exists %s", fullPath.c_str());
		return true;
	}

	size_t position = 0;

#ifdef _WIN32
	// Skip the drive letter when looking for slashes, no need to create C:\.
	if (path.IsAbsolute()) {
		position = 3;
	}
#endif

	while (true) {
		// Find next sub path
		position = fullPath.find_first_of(DIR_SEP_CHRS, position);
		// we're done, yay!
		if (position == fullPath.npos) {
			if (!File::Exists(fullPath))
				return File::CreateDir(Path(fullPath));
			return true;
		}
		std::string subPath = fullPath.substr(0, position);
		if (position != 0 && !File::Exists(subPath)) {
			File::CreateDir(Path(subPath));
		}

		// A safety check
		panicCounter--;
		if (panicCounter <= 0) {
			ERROR_LOG(COMMON, "CreateFullPath: directory structure too deep");
			return false;
		}
		position++;
	}
}

// Deletes a directory filename, returns true on success
bool DeleteDir(const Path &path) {
	switch (path.Type()) {
	case PathType::NATIVE:
		break; // OK
	default:
		return false;
	}
	INFO_LOG(COMMON, "DeleteDir: directory %s", path.c_str());

	// check if a directory
	if (!File::IsDirectory(path))
	{
		ERROR_LOG(COMMON, "DeleteDir: Not a directory %s", path.c_str());
		return false;
	}

#ifdef _WIN32
	if (::RemoveDirectory(path.ToWString().c_str()))
		return true;
#else
	if (rmdir(path.c_str()) == 0)
		return true;
#endif
	ERROR_LOG(COMMON, "DeleteDir: %s: %s", path.c_str(), GetLastErrorMsg().c_str());

	return false;
}

// renames file srcFilename to destFilename, returns true on success 
bool Rename(const Path &srcFilename, const Path &destFilename)
{
	if (srcFilename.Type() != destFilename.Type()) {
		// Impossible.
		return false;
	}

	switch (srcFilename.Type()) {
	case PathType::NATIVE:
		break; // OK
	default:
		return false;
	}

	INFO_LOG(COMMON, "Rename: %s --> %s", srcFilename.c_str(), destFilename.c_str());
#if defined(_WIN32) && defined(UNICODE)
	std::wstring srcw = srcFilename.ToWString();
	std::wstring destw = destFilename.ToWString();
	if (_wrename(srcw.c_str(), destw.c_str()) == 0)
		return true;
#else
	if (rename(srcFilename.c_str(), destFilename.c_str()) == 0)
		return true;
#endif

	ERROR_LOG(COMMON, "Rename: failed %s --> %s: %s", 
			  srcFilename.c_str(), destFilename.c_str(), GetLastErrorMsg().c_str());
	return false;
}

// copies file srcFilename to destFilename, returns true on success 
bool Copy(const Path &srcFilename, const Path &destFilename)
{
	switch (srcFilename.Type()) {
	case PathType::NATIVE:
		break; // OK
	default:
		return false;
	}
	switch (destFilename.Type()) {
	case PathType::NATIVE:
		break; // OK
	default:
		return false;
	}

	INFO_LOG(COMMON, "Copy: %s --> %s", srcFilename.c_str(), destFilename.c_str());
#ifdef _WIN32
#if PPSSPP_PLATFORM(UWP)
	if (CopyFile2(srcFilename.ToWString().c_str(), destFilename.ToWString().c_str(), nullptr))
		return true;
	return false;
#else
	if (CopyFile(srcFilename.ToWString().c_str(), destFilename.ToWString().c_str(), FALSE))
		return true;
#endif
	ERROR_LOG(COMMON, "Copy: failed %s --> %s: %s", 
			srcFilename.c_str(), destFilename.c_str(), GetLastErrorMsg().c_str());
	return false;
#else

	// buffer size
#define BSIZE 4096

	char buffer[BSIZE];

	// Open input file
	FILE *input = OpenCFile(srcFilename, "rb");
	if (!input) {
		ERROR_LOG(COMMON, "Copy: input failed %s --> %s: %s", 
				srcFilename.c_str(), destFilename.c_str(), GetLastErrorMsg().c_str());
		return false;
	}

	// open output file
	FILE *output = OpenCFile(destFilename, "wb");
	if (!output) {
		fclose(input);
		ERROR_LOG(COMMON, "Copy: output failed %s --> %s: %s", 
				srcFilename.c_str(), destFilename.c_str(), GetLastErrorMsg().c_str());
		return false;
	}

	// copy loop
	while (!feof(input)) {
		// read input
		int rnum = fread(buffer, sizeof(char), BSIZE, input);
		if (rnum != BSIZE) {
			if (ferror(input) != 0) {
				ERROR_LOG(COMMON, 
						"Copy: failed reading from source, %s --> %s: %s", 
						srcFilename.c_str(), destFilename.c_str(), GetLastErrorMsg().c_str());
				fclose(input);
				fclose(output);		
				return false;
			}
		}

		// write output
		int wnum = fwrite(buffer, sizeof(char), rnum, output);
		if (wnum != rnum) {
			ERROR_LOG(COMMON, 
					"Copy: failed writing to output, %s --> %s: %s", 
					srcFilename.c_str(), destFilename.c_str(), GetLastErrorMsg().c_str());
			fclose(input);
			fclose(output);				
			return false;
		}
	}
	// close flushes
	fclose(input);
	fclose(output);
	return true;
#endif
}

bool Move(const Path &srcFilename, const Path &destFilename) {
	if (Rename(srcFilename, destFilename)) {
		return true;
	} else if (Copy(srcFilename, destFilename)) {
		return Delete(srcFilename);
	} else {
		return false;
	}
}

// Returns the size of file (64bit)
// TODO: Add a way to return an error.
uint64_t GetFileSize(const Path &filename) {
#if defined(_WIN32) && defined(UNICODE)
	WIN32_FILE_ATTRIBUTE_DATA attr;
	if (!GetFileAttributesEx(filename.ToWString().c_str(), GetFileExInfoStandard, &attr))
		return 0;
	if (attr.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
		return 0;
	return ((uint64_t)attr.nFileSizeHigh << 32) | (uint64_t)attr.nFileSizeLow;
#else
#if __ANDROID__ && __ANDROID_API__ < 21
	struct stat file_info;
	int result = stat(filename.c_str(), &file_info);
#else
	struct stat64 file_info;
	int result = stat64(filename.c_str(), &file_info);
#endif
	if (result != 0) {
		WARN_LOG(COMMON, "GetSize: failed %s: No such file", filename.ToVisualString().c_str());
		return 0;
	}
	if (S_ISDIR(file_info.st_mode)) {
		WARN_LOG(COMMON, "GetSize: failed %s: is a directory", filename.ToVisualString().c_str());
		return 0;
	}
	DEBUG_LOG(COMMON, "GetSize: %s: %lld", filename.ToVisualString().c_str(), (long long)file_info.st_size);
	return file_info.st_size;
#endif
}

uint64_t GetFileSize(FILE *f)
{
	// This will only support 64-bit when large file support is available.
	// That won't be the case on some versions of Android, at least.
#if defined(__ANDROID__) || (defined(_FILE_OFFSET_BITS) && _FILE_OFFSET_BITS < 64)
	int fd = fileno(f);

	off64_t pos = lseek64(fd, 0, SEEK_CUR);
	off64_t size = lseek64(fd, 0, SEEK_END);
	if (size != pos && lseek64(fd, pos, SEEK_SET) != pos) {
		// Should error here.
		return 0;
	}
	return size;
#else
#ifdef _WIN32
	uint64_t pos = _ftelli64(f);
#else
	uint64_t pos = ftello(f);
#endif
	if (fseek(f, 0, SEEK_END) != 0) {
		return 0;
	}
#ifdef _WIN32
	uint64_t size = _ftelli64(f);
	// Reset the seek position to where it was when we started.
	if (size != pos && _fseeki64(f, pos, SEEK_SET) != 0) {
#else
	uint64_t size = ftello(f);
	// Reset the seek position to where it was when we started.
	if (size != pos && fseeko(f, pos, SEEK_SET) != 0) {
#endif
		// Should error here.
		return 0;
	}
	return size;
#endif
}

// creates an empty file filename, returns true on success 
bool CreateEmptyFile(const Path &filename) {

	INFO_LOG(COMMON, "CreateEmptyFile: %s", filename.c_str()); 
	FILE *pFile = OpenCFile(filename, "wb");
	if (!pFile) {
		ERROR_LOG(COMMON, "CreateEmptyFile: failed %s: %s", filename.c_str(), GetLastErrorMsg().c_str());
		return false;
	}
	fclose(pFile);
	return true;
}

// Deletes the given directory and anything under it. Returns true on success.
bool DeleteDirRecursively(const Path &directory) {	
	//Removed check, it prevents the UWP from deleting store downloads
	INFO_LOG(COMMON, "DeleteDirRecursively: %s", directory.c_str());

#ifdef _WIN32

	// Find the first file in the directory.
	WIN32_FIND_DATA ffd;
	HANDLE hFind = FindFirstFile((directory.ToWString() + L"\\*").c_str(), &ffd);
	if (hFind == INVALID_HANDLE_VALUE) {
		return false;
	}
		
	// windows loop
	do {
		const std::string virtualName = ConvertWStringToUTF8(ffd.cFileName);
#else
	struct dirent *result = NULL;
	DIR *dirp = opendir(directory.c_str());
	if (!dirp)
		return false;

	// non windows loop
	while ((result = readdir(dirp))) {
		const std::string virtualName = result->d_name;
#endif
		// check for "." and ".."
		if (((virtualName[0] == '.') && (virtualName[1] == '\0')) ||
			((virtualName[0] == '.') && (virtualName[1] == '.') && 
			 (virtualName[2] == '\0')))
			continue;

		Path newPath = directory / virtualName;
		if (IsDirectory(newPath)) {
			if (!DeleteDirRecursively(newPath)) {
#ifndef _WIN32
				closedir(dirp);
#else
				FindClose(hFind);
#endif
				return false;
			}
		}
		else {
			if (!File::Delete(newPath)) {
#ifndef _WIN32
				closedir(dirp);
#else
				FindClose(hFind);
#endif
				return false;
			}
		}

#ifdef _WIN32
	} while (FindNextFile(hFind, &ffd) != 0);
	FindClose(hFind);
#else
	}
	closedir(dirp);
#endif
	return File::DeleteDir(directory);
}

void OpenFileInEditor(const Path &fileName) {
#if defined(_WIN32)
#if PPSSPP_PLATFORM(UWP)
	// Do nothing.
#else
	ShellExecuteW(nullptr, L"open", fileName.ToWString().c_str(), nullptr, nullptr, SW_SHOW);
#endif
#elif !defined(MOBILE_DEVICE)
	std::string iniFile;
#if defined(__APPLE__)
	iniFile = "open ";
#else
	iniFile = "xdg-open ";
#endif
	iniFile.append(fileName.ToString());
	NOTICE_LOG(BOOT, "Launching %s", iniFile.c_str());
	int retval = system(iniFile.c_str());
	if (retval != 0) {
		ERROR_LOG(COMMON, "Failed to launch ini file");
	}
#endif
}

const Path &GetExeDirectory() {
	static Path ExePath;

	if (ExePath.empty()) {
#ifdef _WIN32
		std::wstring program_path;
		size_t sz;
		do {
			program_path.resize(program_path.size() + MAX_PATH);
			// On failure, this will return the same value as passed in, but success will always be one lower.
			sz = GetModuleFileNameW(nullptr, &program_path[0], (DWORD)program_path.size());
		} while (sz >= program_path.size());

		const wchar_t *last_slash = wcsrchr(&program_path[0], '\\');
		if (last_slash != nullptr)
			program_path.resize(last_slash - &program_path[0] + 1);
		else
			program_path.resize(sz);
		ExePath = Path(program_path);

#elif (defined(__APPLE__) && !PPSSPP_PLATFORM(IOS)) || defined(__linux__) || defined(KERN_PROC_PATHNAME)
		char program_path[4096];
		uint32_t program_path_size = sizeof(program_path) - 1;

#if defined(__linux__)
		if (readlink("/proc/self/exe", program_path, program_path_size) > 0)
#elif defined(__APPLE__) && !PPSSPP_PLATFORM(IOS)
		if (_NSGetExecutablePath(program_path, &program_path_size) == 0)
#elif defined(KERN_PROC_PATHNAME)
		int mib[4] = {
			CTL_KERN,
#if defined(__NetBSD__)
			KERN_PROC_ARGS,
			-1,
			KERN_PROC_PATHNAME,
#else
			KERN_PROC,
			KERN_PROC_PATHNAME,
			-1,
#endif
		};
		size_t sz = program_path_size;

		if (sysctl(mib, 4, program_path, &sz, NULL, 0) == 0)
#else
#error Unmatched ifdef.
#endif
		{
			program_path[sizeof(program_path) - 1] = '\0';
			char *last_slash = strrchr(program_path, '/');
			if (last_slash != nullptr)
				*last_slash = '\0';
			ExePath = Path(program_path);
		}
#endif
	}

	return ExePath;
}

IOFile::IOFile() {}

IOFile::IOFile(std::FILE* file)
	: m_file(file)
{}

IOFile::IOFile(const std::string &filename, const char openmode[]) {
	Open(filename, openmode);
}

IOFile::IOFile(const Path &filename, const char openmode[])  {
	Open(filename.ToString(), openmode);
}

IOFile::~IOFile() {
	Close();
}

bool IOFile::Open(const std::string& filename, const char openmode[])
{
	Close();
	m_file = File::OpenCFile(Path(filename), openmode);
	m_good = IsOpen();
	return m_good;
}

bool IOFile::Open(const Path& filename, const char openmode[])
{
	Close();
	m_file = File::OpenCFile(filename, openmode);
	m_good = IsOpen();
	return m_good;
}

bool IOFile::Close()
{
	if (!IsOpen() || 0 != std::fclose(m_file))
		m_good = false;

	m_file = NULL;
	return m_good;
}

std::FILE* IOFile::ReleaseHandle()
{
	std::FILE* const ret = m_file;
	m_file = NULL;
	return ret;
}

void IOFile::SetHandle(std::FILE* file)
{
	Close();
	Clear();
	m_file = file;
}

uint64_t IOFile::GetSize()
{
	if (IsOpen())
		return File::GetFileSize(m_file);
	else
		return 0;
}

bool IOFile::Seek(int64_t off, int origin)
{
	if (!IsOpen() || 0 != fseeko(m_file, off, origin))
		m_good = false;

	return m_good;
}

uint64_t IOFile::Tell()
{	
	if (IsOpen())
		return ftello(m_file);
	else
		return -1;
}

bool IOFile::Flush()
{
	if (!IsOpen() || 0 != std::fflush(m_file))
		m_good = false;

	return m_good;
}

bool IOFile::Resize(uint64_t size)
{
	if (!IsOpen() || 0 !=
#ifdef _WIN32
		// ector: _chsize sucks, not 64-bit safe
		// F|RES: changed to _chsize_s. i think it is 64-bit safe
		_chsize_s(_fileno(m_file), size)
#else
		// TODO: handle 64bit and growing
		ftruncate(fileno(m_file), size)
#endif
	)
		m_good = false;

	return m_good;
}

bool ReadFileToString(bool text_file, const Path &filename, std::string &str) {
	FILE *f = File::OpenCFile(filename, text_file ? "r" : "rb");
	if (!f)
		return false;
	// Warning: some files, like in /sys/, may return a fixed size like 4096.
	size_t len = (size_t)File::GetFileSize(f);
	bool success;
	if (len == -1) {
		size_t totalSize = 1024;
		size_t totalRead = 0;
		do {
			totalSize *= 2;
			str.resize(totalSize);
			totalRead += fread(&str[totalRead], 1, totalSize - totalRead, f);
		} while (totalRead == totalSize);
		str.resize(totalRead);
		success = true;
	} else {
		str.resize(len);
		size_t totalRead = fread(&str[0], 1, len, f);
		str.resize(totalRead);
		// Allow less, because some system files will report incorrect lengths.
		success = totalRead <= len;
	}
	fclose(f);
	return success;
}

// This is an odd one, mainly used for asset reading, so doesn't really
// need to support Path.
uint8_t *ReadLocalFile(const char *filename, size_t * size) {
	FILE *file = File::OpenCFile(Path(filename), "rb");
	if (!file) {
		*size = 0;
		return nullptr;
	}
	fseek(file, 0, SEEK_END);
	size_t f_size = ftell(file);
	if ((long)f_size < 0) {
		*size = 0;
		fclose(file);
		return nullptr;
	}
	fseek(file, 0, SEEK_SET);
	uint8_t *contents = new uint8_t[f_size + 1];
	if (fread(contents, 1, f_size, file) != f_size) {
		delete[] contents;
		contents = nullptr;
		*size = 0;
	} else {
		contents[f_size] = 0;
		*size = f_size;
	}
	fclose(file);
	return contents;
}

bool WriteStringToFile(bool text_file, const std::string &str, const Path &filename) {
	FILE *f = File::OpenCFile(filename, text_file ? "w" : "wb");
	if (!f)
		return false;
	size_t len = str.size();
	if (len != fwrite(str.data(), 1, str.size(), f))
	{
		fclose(f);
		return false;
	}
	fclose(f);
	return true;
}

bool WriteDataToFile(bool text_file, const void* data, const unsigned int size, const Path &filename) {
	FILE *f = File::OpenCFile(filename, text_file ? "w" : "wb");
	if (!f)
		return false;
	size_t len = size;
	if (len != fwrite(data, 1, len, f))
	{
		fclose(f);
		return false;
	}
	fclose(f);
	return true;
}

}  // namespace File
