#include <windows.h>
#include <iostream>
#include <fstream>
#include <filesystem>

#include "gtest/gtest.h"

#include "common/SubProcess.h"
#include "common/run_cmd.h"
#include "include/uuid.h"

#define DEFAULT_MOUNTPOINT "X:\\"
#define MOUNT_POLL_ATTEMPT 10
#define MOUNT_POLL_INTERVAL_MS 1000

std::string get_uuid() {
    uuid_d suffix;
    suffix.generate_random();

    return suffix.to_string();
}

void write_file(std::string file_path, std::string data, bool expect_failure) {
    std::ofstream file;
    file.open(file_path);

    if(expect_failure){
        ASSERT_FALSE(file.is_open());
        return;
    }

    ASSERT_TRUE(file.is_open())
        << "Failed to open file: " << file_path;
    file << data;
    file.flush();

    file.close();
}

std::string read_file(std::string file_path) {
    std::ifstream file;
    file.open(file_path);
    std::string content((std::istreambuf_iterator<char>(file)),
                 std::istreambuf_iterator<char>());
    file.close();

    return content;
}

void check_write_file(std::string file_path, std::string data) {
    write_file(file_path, data, false);
    ASSERT_EQ(read_file(file_path), data);
}

int wait_for_mount(std::string mount_path) {
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

void map_dokan(SubProcess** mount, const char* mountpoint) {
    SubProcess* new_mount = new SubProcess("ceph-dokan");
    new_mount->add_cmd_args("map", "-l", mountpoint, NULL);

    *mount = new_mount;
    ASSERT_EQ(new_mount->spawn(), 0);
    ASSERT_EQ(wait_for_mount(mountpoint), 0);
}

void map_dokan_read_only(SubProcess** mount, const char* mountpoint) {
    SubProcess* new_mount = new SubProcess("ceph-dokan");
    new_mount->add_cmd_args("map", "--read-only", "-l", mountpoint, NULL);

    *mount = new_mount;
    ASSERT_EQ(new_mount->spawn(), 0);
    ASSERT_EQ(wait_for_mount(mountpoint), 0);
    std::cerr << mountpoint << " mounted in read-only mode"<< std::endl;
}

void unmap_dokan(SubProcess* mount, const char* mountpoint) {
    std::string ret = run_cmd("ceph-dokan", "unmap", "-l", mountpoint, (char*)NULL);
    ASSERT_EQ(ret, "")
        << "Failed unmapping: " << mountpoint;
    std::cerr<< "Unmounted: " << mountpoint << std::endl;

    ASSERT_EQ(mount->join(), 0);
}

static SubProcess* shared_mount = nullptr;

class DokanTests : public testing::Test
{
protected:

    static void SetUpTestSuite() {
        map_dokan(&shared_mount, DEFAULT_MOUNTPOINT);
    }

    static void TearDownTestSuite() {
        if (shared_mount) {
            unmap_dokan(shared_mount, DEFAULT_MOUNTPOINT);
        }
        shared_mount = nullptr;
    }
};

TEST_F(DokanTests, test_mount) {
    std::string mountpoint = "Y:\\";
    SubProcess* mount = nullptr;
    map_dokan(&mount, mountpoint.c_str());
    unmap_dokan(mount, mountpoint.c_str());
}

TEST_F(DokanTests, test_mount_read_only) {
    std::string mountpoint = "Z:\\";
    std::string data = "abc123";
    std::string success_file_path = "ro_success_" + get_uuid();
    std::string failed_file_path = "ro_fail_" + get_uuid();

    SubProcess* mount = nullptr;
    map_dokan(&mount, mountpoint.c_str());

    check_write_file(mountpoint + success_file_path, data);
    EXPECT_TRUE(std::filesystem::exists(mountpoint + success_file_path));

    unmap_dokan(mount, mountpoint.c_str());

    mount = nullptr;
    map_dokan_read_only(&mount, mountpoint.c_str());

    write_file(mountpoint + failed_file_path, data, true);
    EXPECT_FALSE(std::filesystem::exists(mountpoint + failed_file_path));

    EXPECT_TRUE(std::filesystem::exists(mountpoint + success_file_path));
    EXPECT_EQ(read_file(mountpoint + success_file_path), data);
    std::string exception_msg("filesystem error: cannot remove: No such device ["
                              + mountpoint + success_file_path + "]");
    EXPECT_THROW({
        try {
            std::filesystem::remove(mountpoint + success_file_path);
        } catch(const std::filesystem::filesystem_error &e) {
            EXPECT_STREQ(e.what(), exception_msg.c_str());
            throw;
        }
    }, std::filesystem::filesystem_error);
    unmap_dokan(mount, mountpoint.c_str());

    map_dokan(&mount, mountpoint.c_str());
    EXPECT_TRUE(std::filesystem::exists(mountpoint + success_file_path));
    EXPECT_TRUE(std::filesystem::remove(mountpoint + success_file_path));
    unmap_dokan(mount, mountpoint.c_str());
}

TEST_F(DokanTests, test_create_file) {
    std::string file_path = DEFAULT_MOUNTPOINT"test_create_" + get_uuid();
    HANDLE hFile = CreateFile(
        file_path.c_str(),
        GENERIC_WRITE, // open for writing
        0,             // sharing mode, none in this case
        0,             // use default security descriptor
        CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE,
        0);

    EXPECT_NE(hFile, INVALID_HANDLE_VALUE)
        << "Could not open file: "
        << DEFAULT_MOUNTPOINT"test_create.txt "
        << "err: " << GetLastError() << std::endl;
        
    EXPECT_NE(CloseHandle(hFile), 0);
    
    // FILE_FLAG_DELETE_ON_CLOSE is used
    EXPECT_FALSE(std::filesystem::exists(file_path));
}

TEST_F(DokanTests, test_io) {
    std::string data = "abcdef";
    std::string file_path = "test_io_" + get_uuid();

    std::string mountpoint = "I:\\";
    SubProcess* mount = nullptr;
    map_dokan(&mount, mountpoint.c_str());

    check_write_file(mountpoint + file_path, data);
    EXPECT_TRUE(std::filesystem::exists(mountpoint + file_path));
    unmap_dokan(mount, mountpoint.c_str());

    mountpoint = "O:\\";
    mount = nullptr;
    map_dokan(&mount, mountpoint.c_str());

    EXPECT_TRUE(std::filesystem::exists(mountpoint + file_path));
    EXPECT_EQ(data, read_file(mountpoint + file_path));
    EXPECT_TRUE(std::filesystem::remove((mountpoint + file_path).c_str()));
    EXPECT_FALSE(std::filesystem::exists(mountpoint + file_path));
    unmap_dokan(mount, mountpoint.c_str());
}

TEST_F(DokanTests, test_subfolders) {
    std::string base_dir_path = DEFAULT_MOUNTPOINT"base_dir_"
                                + get_uuid();
    std::string sub_dir_path = base_dir_path
                               + "\\test_sub_dir" + get_uuid();

    std::string base_dir_file = base_dir_path 
                                + "\\file_" + get_uuid();
    std::string sub_dir_file = sub_dir_path 
                                + "\\file_" + get_uuid();

    std::string data = "abc";

    ASSERT_EQ(std::filesystem::create_directory(base_dir_path), true);
    EXPECT_TRUE(std::filesystem::exists(base_dir_path));

    ASSERT_EQ(std::filesystem::create_directory(sub_dir_path), true);
    EXPECT_TRUE(std::filesystem::exists(sub_dir_path));

    check_write_file(base_dir_file, data);
    EXPECT_TRUE(std::filesystem::exists(base_dir_file));

    check_write_file(sub_dir_file, data);
    EXPECT_TRUE(std::filesystem::exists(sub_dir_file));

    EXPECT_TRUE(std::filesystem::remove((sub_dir_file).c_str()))
        << "Failed to remove file: " << sub_dir_file;
    EXPECT_FALSE(std::filesystem::exists(sub_dir_file));

    // Remove empty dir
    EXPECT_TRUE(std::filesystem::remove((sub_dir_path).c_str()))
        << "Failed to remove directory: " << sub_dir_path;
    EXPECT_FALSE(std::filesystem::exists(sub_dir_file));

    EXPECT_NE(std::filesystem::remove_all((base_dir_path).c_str()), 0)
        << "Failed to remove directory: " << base_dir_path;
    EXPECT_FALSE(std::filesystem::exists(sub_dir_file));
}

TEST_F(DokanTests, test_cleanup) {
    std::cerr << "NO-OP" << std::endl;
}

// implemented in test_io
TEST_F(DokanTests, test_flush) {
    std::cerr << "NO-OP" << std::endl;
}

TEST_F(DokanTests, test_find_files) {
    std::cerr << "NO-OP" << std::endl;
}

// might be included in test_io
TEST_F(DokanTests, test_move_file) {
    std::cerr << "NO-OP" << std::endl;
}

TEST_F(DokanTests, test_set_eof) {
    std::cerr << "NO-OP" << std::endl;
}

TEST_F(DokanTests, test_allocation_size) {
    std::cerr << "NO-OP" << std::endl;
}

TEST_F(DokanTests, test_file_info) {
    std::cerr << "NO-OP" << std::endl;
}

TEST_F(DokanTests, test_set_file_attr) {
    std::cerr << "NO-OP" << std::endl;
}

TEST_F(DokanTests, test_file_time) {
    std::cerr << "NO-OP" << std::endl;
}

TEST_F(DokanTests, test_file_security) {
    std::cerr << "NO-OP" << std::endl;
}

TEST_F(DokanTests, test_volume_info) {
    std::cerr << "NO-OP" << std::endl;
}

TEST_F(DokanTests, test_get_free_space) {
    std::cerr << "NO-OP" << std::endl;
}