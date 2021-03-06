#include <iostream>
#include <string>
#include <errno.h>
#include <vector>
#include <thread>
#include <memory>
#include <sstream>
#include <string>

#include "oelogo.h"
#include "oni.hpp"
#include "onix.hpp"

// Dump raw device streams to files?
//#define DUMPFILES

#ifdef DUMPFILES
std::vector<FILE *> dump_files;
#endif

// Windows- and UNIX-specific includes etc
#ifdef _WIN32
#include <windows.h>
//#pragma comment(lib, "liboni")
#include <stdio.h>
#include <stdlib.h>
#else
#include <unistd.h>
#include <pthread.h>
#endif

volatile int quit = 0;
volatile int display = 0;
int running = 1;

int parse_reg_cmd(const char *cmd, long *values, int len)
{
    char *end;
    int k = 0;
    for (long i = strtol(cmd, &end, 10); cmd != end;
         i = strtol(cmd, &end, 10)) {
        cmd = end;
        if (errno == ERANGE) {
            return -1;
        }

        values[k++] = i;
        if (k == 3)
            break;
    }

    if (k < len)
        return -1;

    return 0;
}

void print_dev_table(oni::device_map_t devices)
{
    // Show device table
    printf("   +--------------------+-------+-------+-------+---------------------\n");
    printf("   |        \t\t|  \t|Read\t|Wrt. \t|     \n");
    printf("   |Dev. idx\t\t|ID\t|size\t|size \t|Desc.\n");
    printf("   +--------------------+-------+-------+-------+---------------------\n");

    int k = 0;
    for (const auto &d : devices) {

        const char *dev_str = onix::device_str(d.second.id);

        printf("%02d |%05d: 0x%02x.0x%02x\t|%d\t|%u\t|%u\t|%s\n",
               k++,
               d.second.idx,
               (uint8_t)(d.second.idx >> 8),
               (uint8_t)d.second.idx,
               d.second.id,
               d.second.read_size,
               d.second.write_size,
               dev_str);
    }

    printf("   +--------------------+-------+-------+-------+---------------------\n");
}

void data_loop(std::shared_ptr<oni::context_t> ctx)
{
    unsigned long counter = 0;
    int rc = 0;
    const auto dev_map = ctx->device_map();

    try {

        while (rc == 0 && !quit) {

            auto frame = ctx->read_frame();


            auto data = frame.data<uint16_t>();

#ifdef DUMPFILES
            fwrite(data.data(), sizeof(uint16_t), data.size(), dump_files[frame.device_index()]);
#endif

            if (display && counter % 1000 == 0) {

                std::cout << "\t [" << frame.time() << "] Dev: " << frame.device_index() << " ("
                          << onix::device_str(dev_map.at(frame.device_index()).id)
                          << ")\n";

                std::cout << "\tData: [";

                for (const auto &d : data)
                    std::cout << d << " ";

                std::cout << "]\n";
            }

            counter++;
        }

    } catch (const oni::error_t &ex) {
        quit = true;
        std::cerr << ex.what() << "\n";
    }
}

int main(int argc, char *argv[])
{
    try {
        std::cout << oe_logo_med;

        oni_size_t block_read_size = 1024;
        oni_size_t block_write_size = 1024;
        int host_idx = -1;
        std::string driver;

        switch (argc) {

            case 5:
                block_write_size = atoi(argv[4]);
            case 4:
                block_read_size = atoi(argv[3]);
            case 3:
                host_idx = atoi(argv[2]);
            case 2:
                driver = argv[1];
                break;
            default:
                std::cout << "usage:\n";
                std::cout << "\thost driver [host_index] ...\n";
                exit(1);
        }

        // Create context
        auto ctx = std::make_shared<oni::context_t>(driver.c_str(), host_idx);

        // Examine device map
        auto dev_map = ctx->device_map();

        // Print device table
        print_dev_table(dev_map);

#ifdef DUMPFILES
        for (int i = 0; i < dev_map.size(); i++) {

            // Open dump files
            std::stringstream ss;
            ss << "idx-" << i << "_id-" << dev_map[i].id << ".raw";
            dump_files.emplace_back();
            dump_files.back() = fopen(ss.str().c_str(), "wb");
        }
#endif

        std::cout << "Max. read frame size: "
                  << ctx->get_opt<uint32_t>(ONI_OPT_MAXREADFRAMESIZE)
                  << " bytes\n";

        std::cout << "Setting block read size to: " << block_read_size << " bytes\n";
        ctx->set_opt(ONI_OPT_BLOCKREADSIZE, block_read_size);

        std::cout << "Block read size: "
                  << ctx->get_opt<oni_size_t>(ONI_OPT_BLOCKREADSIZE)
                  << " bytes\n";

        std::cout << "Setting write pre-allocation buffer to: " << block_write_size
                  << " bytes\n";
        ctx->set_opt(ONI_OPT_BLOCKWRITESIZE, block_write_size);

        std::cout << "Write pre-allocation size: "
                  << ctx->get_opt<oni_size_t>(ONI_OPT_BLOCKWRITESIZE)
                  << " bytes\n";

        std::cout
            << "System clock rate: " << ctx->get_opt<uint32_t>(ONI_OPT_SYSCLKHZ)
            << " Hz\n";

        // Start acquisition
        ctx->set_opt(ONI_OPT_RUNNING, 1);

        // Generate data thread and continue here config/signal handling in
        // parallel
        std::thread tid(data_loop, ctx);

        // Read stdin
        std::string cmd;
        while (cmd != "q") {

            std::cout << "Enter a command and press enter:\n"
                      << "\td - toggle 1/1000 display\n"
                      << "\tt - print device table\n"
                      << "\tp - toggle running register\n"
                      << "\tr - read from device register\n"
                      << "\tw - write to device register\n"
                      << "\tx - issue a hardware reset\n"
                      << "\tq - quit\n"
                      << ">>> ";

            std::getline(std::cin, cmd);

            if (cmd == "p") {
                running = (running == 1) ? 0 : 1;
                ctx->set_opt(ONI_OPT_RUNNING, running);

                if (running)
                    std::cout << "Running.\n";
                else
                    std::cout << "Paused\n";
            } else if (cmd == "x") {
                ctx->set_opt(ONI_OPT_RESET, 1);
            } else if (cmd == "d") {
                display = (display == 0) ? 1 : 0;
            } else if (cmd == "r") {
                std::cout << "Read a device register.\n"
                          << "Enter: dev_idx reg_addr\n"
                          << ">>> ";

                // Read the command
                std::string reg_cmd;
                std::getline(std::cin, reg_cmd);

                // Parse the command string
                long values[2];
                auto rc = parse_reg_cmd(reg_cmd.c_str(), values, 2);
                if (rc == -1) {
                    std::cerr << "Error: bad command\n";
                    continue;
                }

                auto dev_idx = (oni_dev_idx_t)values[0];
                auto addr = (oni_size_t)values[1];

                auto val = ctx->read_reg(dev_idx, addr);
                std::cout << "Reg. value: " << val << "\n";
            } else if (cmd == "t") {
                print_dev_table(dev_map);
            } else if (cmd == "w") {
                std::cout << "Write to a device register.\n"
                          << "Enter dev_idx reg_addr reg_val\n"
                          << ">>> ";

                // Read the command
                std::string reg_cmd;
                std::getline(std::cin, reg_cmd);

                // Parse the command string
                long values[3];
                auto rc = parse_reg_cmd(reg_cmd.c_str(), values, 3);
                if (rc == -1) {
                    std::cerr << "Error: bad command\n";
                    continue;
                }

                auto dev_idx = (oni_dev_idx_t)values[0];
                auto addr = (oni_reg_addr_t)values[1];
                auto val = (oni_reg_val_t)values[2];

                ctx->write_reg(dev_idx, addr, val);
                std::cout << "Success\n";
            }
        }

        //ctx->write<uint32_t>(8, std::vector<uint32_t>{1, 2});

        // Join data and signal threads
        quit = 1;
        tid.join();

#ifdef DUMPFILES
        // Close dump files
        for (int dev_idx = 0; dev_idx < dev_map.size(); dev_idx++)
            fclose(dump_files[dev_idx]);
#endif
    } catch (oni::error_t err) {
        std::cerr << err.what();
        return -1;
    }

    return 0;
}
