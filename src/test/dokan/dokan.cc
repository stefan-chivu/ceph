#include <windows.h>
#include <iostream>
#include <filesystem>

#include "gtest/gtest.h"

#include "common/SubProcess.h"
#include "common/run_cmd.h"
#include "include/uuid.h"

#define DEFAULT_MOUNTPOINT "X:\\"
#define MOUNT_POLL_ATTEMPT 10
#define MOUNT_POLL_INTERVAL_MS 1000

std::string get_uuid(){
    uuid_d suffix;
    suffix.generate_random();

    return suffix.to_string();
}

int wait_for_mount(std::string mount_path){
    std::cerr << "Waiting for mount: " << mount_path << std::endl;

    HANDLE hFile;
    int attempts = 0;
    do {
        hFile= CreateFile(
            mount_path.c_str(),
            GENERIC_READ,          // open for reading
            FILE_SHARE_READ,       // share for reading
            NULL,                  // default security
            OPEN_EXISTING,         // existing file only
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS, // normal file
            NULL);

        attempts++;
        if (attempts < MOUNT_POLL_ATTEMPT && hFile == INVALID_HANDLE_VALUE)
            Sleep(MOUNT_POLL_INTERVAL_MS);
    } while (hFile == INVALID_HANDLE_VALUE && attempts < MOUNT_POLL_ATTEMPT);

    if (hFile == INVALID_HANDLE_VALUE) {
        std::cerr << "Timed out waiting for ceph-dokan mount: " << mount_path
                  << ", err: " << GetLastError() << std::endl;
        return -ETIMEDOUT;
    }

    std::cerr << "Successfully mounted: " << mount_path << std::endl;

    return 0;
}

static SubProcess* shared_mount = nullptr;

class DokanTests : public testing::Test
{
protected:

    static void SetUpTestSuite() {
        if (shared_mount == nullptr) {
            shared_mount = new SubProcess("ceph-dokan");
            shared_mount->add_cmd_args("map","-l", DEFAULT_MOUNTPOINT, NULL);
            ASSERT_EQ(shared_mount->spawn(), 0);

            ASSERT_EQ(wait_for_mount(DEFAULT_MOUNTPOINT), 0);
        }
    }

    static void TearDownTestSuite() {
        if (shared_mount) {
            std::string ret = run_cmd("ceph-dokan", "unmap", "-l", DEFAULT_MOUNTPOINT, (char*)NULL);
            ASSERT_EQ(ret, "");
            std::cerr<<"Unmounted: " << DEFAULT_MOUNTPOINT << std::endl;

            ASSERT_EQ(shared_mount->join(), 0);
            delete shared_mount;
        }
        shared_mount = nullptr;
    }
};

TEST_F(DokanTests,test_mount){
    SubProcess mount("ceph-dokan");
    std::string mountpoint = "Y:\\";

    mount.add_cmd_args("-l", mountpoint.c_str(), NULL);
    ASSERT_EQ(mount.spawn(), 0);

    ASSERT_EQ(wait_for_mount(mountpoint), 0);

    std::string ret = run_cmd("ceph-dokan", "unmap", "-l", mountpoint.c_str(), (char*)NULL);
    ASSERT_EQ(ret, "");
    std::cerr<<"Unmounted: " << mountpoint << std::endl;

    ASSERT_EQ(mount.join(), 0);
}

TEST_F(DokanTests,test_create_file)
{
    std::string file_path = DEFAULT_MOUNTPOINT"test_create_" + get_uuid();
    HANDLE hFile = CreateFile(
        file_path.c_str(),
        GENERIC_WRITE, // open for writing
        0,             // sharing mode, none in this case
        0,             // use default security descriptor
        CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE,
        0);

    if (hFile == INVALID_HANDLE_VALUE) {
        std::cerr << "Could not open file: "
                  << DEFAULT_MOUNTPOINT"test_create.txt "
                  << "err: " << GetLastError() << std::endl;
    }

    EXPECT_NE(hFile, INVALID_HANDLE_VALUE);
    EXPECT_NE(CloseHandle(hFile), 0);
    
    // FILE_FLAG_DELETE_ON_CLOSE is used
    EXPECT_FALSE(std::filesystem::exists(file_path));
}

TEST_F(DokanTests,test_write_file)
{
    char data[7] = "abcdef";
    std::string file_path = DEFAULT_MOUNTPOINT"test_write_" + get_uuid();
    DWORD dwBytesToWrite = (DWORD)strlen(data);
    DWORD dwBytesWritten = 0;
    BOOL bErrorFlag = FALSE;

    HANDLE hFile = CreateFile(
        file_path.c_str(),
        GENERIC_WRITE,         // open for writing
        0,                     // do not share
        0,                     // default security
        CREATE_NEW,            // create new file only
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, // normal file
        0);                    // no attr. template
    
    if (hFile == INVALID_HANDLE_VALUE) {
        std::cerr << "Could not open file: "
                  << DEFAULT_MOUNTPOINT"test_write.txt "
                  << "err: " << GetLastError() << std::endl;
    }

    EXPECT_NE(hFile, INVALID_HANDLE_VALUE);

    bErrorFlag = WriteFile( 
        hFile,           // open file handle
        data,            // start of data to write
        dwBytesToWrite,  // number of bytes to write
        &dwBytesWritten, // number of bytes that were written
        NULL);           // no overlapped structure

    if (FALSE == bErrorFlag) {
        std::cerr << "Terminal failure: Unable to write to file.\n";
        EXPECT_NE(FALSE, bErrorFlag);
    } else {
        EXPECT_EQ(dwBytesWritten, dwBytesToWrite);
    }

    EXPECT_NE(CloseHandle(hFile), 0);

    // FILE_FLAG_DELETE_ON_CLOSE is used
    EXPECT_FALSE(std::filesystem::exists(file_path));
}