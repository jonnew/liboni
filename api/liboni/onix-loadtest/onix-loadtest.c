#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <math.h>

#include "oni.h"
#include "onix.h"
#include "oelogo.h"

#ifdef _WIN32
#include <windows.h>
#pragma comment(lib, "liboni")
#include <stdio.h>
#include <stdlib.h>
#else
#include <unistd.h>
#endif

//default values
#define DEF_BLOCK_READ_SIZE 2048;
#define DEF_MEM_STEPS_TIMEOUT 10
#define DEF_LOADTEST_BLOCK_SIZE 100
#define DEF_LOADTEST_START_RATE 100

#define MEM_UPDATE_RATE 1

// Global state
oni_ctx ctx = NULL;
oni_size_t num_devs = 0;
oni_device_t *devices = NULL;
oni_device_t *memusage = NULL;
oni_device_t *loadtest = NULL;
oni_size_t block_read_size = DEF_BLOCK_READ_SIZE;
oni_reg_val_t loadtest_size = DEF_LOADTEST_BLOCK_SIZE;
uint32_t threshold = 0;
int num_steps = DEF_MEM_STEPS_TIMEOUT;
oni_reg_val_t start_rate = DEF_LOADTEST_START_RATE;

inline void error_exit(int rc, const char* str) {
    printf(str);
    printf("Error: %s (%d)\n", oni_error_str(rc), rc);
    if (devices != NULL)
        free(devices);
    if (ctx != NULL)
        oni_destroy_ctx(ctx);
    exit(1);
}

void reset_board(int setwidth) 
{
    int rc = ONI_ESUCCESS;
    oni_reg_val_t val = 1;
    rc = oni_set_opt(ctx, ONI_OPT_RESET, &val, sizeof(val));
    if (rc)
        error_exit(rc, "Error resetting board\n");

    if (setwidth == 1) {
        rc = oni_set_opt(ctx,
                         ONI_OPT_BLOCKREADSIZE,
                         &block_read_size,
                         sizeof(block_read_size));
        if (rc)
            error_exit(rc, "Error setting block read size\n");
    }
}

float bandwidth(oni_reg_val_t hz) {
    return (2.0 * (float)loadtest_size + 8.0) * hz;
}

int main(int argc, char* argv[]) 
{
    oni_reg_val_t val;

    switch (argc) {
        case 6:
            start_rate = atoi(argv[5]);
        case 5:
            threshold = atoi(argv[4]) >> 2;
        case 4:
            num_steps = atoi(argv[3]);
        case 3:
            loadtest_size = atoi(argv[2]);
        case 2:
            block_read_size = atoi(argv[1]);
        case 1:
            break;
        default:
        printf("Usage:\n\t %s [block_read_size] [loadtest_frame_words] [timeout_seconds] [threshold_bytes] [start_rate_hz]\n", argv[0]);
        exit(1);
    }

    if (threshold == 0)
        threshold = 100 * loadtest_size;

    printf(oe_logo_med);
    printf("Bandwdith tester. Usage:\n\t %s [block_read_size] "
           "[loadtest_frame_words] [timeout_seconds] [threshold_32bit_words] [start_rate_hz]\n",
           argv[0]);
    printf("Selected settings:\nBlock read size: %d Bytes\nLoad tester words "
           "per frame %d 16bit-words\nTimeout: %d seconds\nBuffer threshold: "
           "%d bytes\n\n",
           block_read_size,
           loadtest_size,
           num_steps,
           threshold << 2);

    // Return code
    int rc = ONI_ESUCCESS;

    // Generate context
    ctx = oni_create_ctx("riffa");
    if (!ctx) {
        printf("Failed to create context\n");
        exit(EXIT_FAILURE);
    }

    // Initialize context and discover hardware
    rc = oni_init_ctx(ctx, -1);
    if (rc) {
        printf("Error: %s\n", oni_error_str(rc));
        exit(1);
    }
    assert(rc == 0);

    reset_board(0);

    // Examine device table
    size_t num_devs_sz = sizeof(num_devs);
    oni_get_opt(ctx, ONI_OPT_NUMDEVICES, &num_devs, &num_devs_sz);

    // Get the device table
    size_t devices_sz = sizeof(oni_device_t) * num_devs;
    devices = (oni_device_t *)malloc(devices_sz);
    if (devices == NULL) {
        exit(EXIT_FAILURE);
    }
    oni_get_opt(ctx, ONI_OPT_DEVICETABLE, devices, &devices_sz);

    //look for load test and memory devices
    for (oni_size_t i = 0; i < num_devs; i++) {
        if (devices[i].id == ONIX_LOADTEST)
            loadtest = &devices[i];
        if (devices[i].id == ONIX_MEMUSAGE)
            memusage = &devices[i];
    }

    if (loadtest == NULL || memusage == NULL) {
        if (loadtest == NULL)
            printf("Load testing device not found\n");
        if (memusage == NULL)
            printf("Memory usage device not found\n");
        free(devices);
        exit(1);
    }

    oni_reg_val_t loadtest_clk, memusage_clk;

    rc = oni_read_reg(ctx, loadtest->idx, 2, &loadtest_clk);
    if (rc)
        error_exit(rc, "Error reading loadtest clock\n");
    rc = oni_read_reg(ctx, memusage->idx, 2, &memusage_clk);
    if (rc)
        error_exit(rc, "Error reading memusage clock\n");

    printf("Load tester internal clock: %u\n\n", loadtest_clk);

    val = 1;
    rc = oni_write_reg(ctx, loadtest->idx, 0, val);
    if (rc)
        error_exit(rc, "Error enabling load test device\n");
    rc = oni_write_reg(ctx, memusage->idx, 0, val);
    if (rc)
        error_exit(rc, "Error enabling memory usage device\n");

    oni_reg_val_t memusage_div = memusage_clk / MEM_UPDATE_RATE;

    rc = oni_write_reg(ctx, memusage->idx, 1, memusage_div);
    if (rc)
        error_exit(rc, "Error setting memory usage clock divisor\n");

    val = loadtest_size;
    rc = oni_write_reg(ctx, loadtest->idx, 3, val);
    if (rc)
        error_exit(rc, "Error setting load test frame size\n");

    oni_reg_val_t loadtest_hz, loadtest_hz_last;
    loadtest_hz = start_rate;
    loadtest_hz_last = start_rate;
 

    int done = 0;
    int mode = 0;
    oni_reg_val_t loadtest_div;
    uint32_t *memvalues = malloc(sizeof(uint32_t) * num_steps);
    oni_reg_val_t hz_step = 0;

    while (!done) {
        loadtest_div = loadtest_clk / loadtest_hz;
        printf("%u Hz (%u Hz) (%.0f MB/s): ", loadtest_hz, loadtest_clk/loadtest_div, bandwidth(loadtest_clk/loadtest_div)/1048576);
        rc = oni_write_reg(ctx, loadtest->idx, 1, loadtest_div);
        if (rc)
            error_exit(rc, "Error setting load test clock divisor\n");
        reset_board(1);

         //start acquiring
        val = 1;
        rc = oni_set_opt(ctx, ONI_OPT_RUNNING, &val, sizeof(val));
        if (rc)
            error_exit(rc, "Error starting acquisition\n");

        int run = 1;
        int step = 0;
        while (run) {
            oni_frame_t *frame = NULL;
            rc = oni_read_frame(ctx, &frame);
            if (rc < 0)
                error_exit(rc, "Error reading frame\n");

            if (frame->dev_idx == memusage->idx) {
                uint16_t* data = (uint16_t *)frame->data;
                uint32_t usage = ((uint32_t)data[4] << 16) | ((uint32_t)data[5]);
                memvalues[step] = usage;
                if (usage < threshold) {
                    step++;
                    if (step == num_steps) {
                        run = 0;
                        val = 0;
                    }
                } else {
                    run = 0;
                }
            }
            oni_destroy_frame(frame);        
        }
        val = 0;
        rc = oni_set_opt(ctx, ONI_OPT_RUNNING, &val, sizeof(val));
        if (rc)
            error_exit(rc, "Error stopping acquisition\n");
        
        if (step < num_steps) { //memory filled
            printf("OVERFLOW at step %d values ", step);
            for (int i = 0; i <= step; i++)
                printf("%u ", memvalues[i]);
            printf(" (32-bit words)\n");
            mode++;
            if (mode == 1) hz_step = loadtest_hz_last;
            else hz_step = hz_step / 10;
            loadtest_hz = loadtest_hz_last;
        } else {
            float mean = 0;
            printf("OK. Mem usage: ");
            for (int i = 0; i < num_steps; i++) {
                printf("%u ", memvalues[i]);
                mean += memvalues[i];
            }
            mean /= num_steps;
            printf("Mean: %f (32-bit words)\n", mean);
            loadtest_hz_last = loadtest_hz;
        }

        uint64_t loadtest_hz_tmp;
        switch (mode) {
            case 0:
                loadtest_hz_tmp = (uint64_t)loadtest_hz * 10;
                break;
            case 1:
            case 2:
                loadtest_hz_tmp = (uint64_t)loadtest_hz + hz_step;
                break;
            default:
                done = 1;
        } 
        if (loadtest_hz_tmp > loadtest_clk) {
            printf("Unable to continue. Next value of %llu Hz is faster than the "
                   "device capabilities. Please adjust threshold and/or "
                   "timeout\n", loadtest_hz_tmp);
            done = 1;
        } else
            loadtest_hz = loadtest_hz_tmp;

    }
    printf("Last good value: %u Hz %.0f B/s\n",
           loadtest_hz_last,
           bandwidth(loadtest_hz_last)); 

    free(memvalues);
    oni_destroy_ctx(ctx);
    free(devices);

    return 0;
}