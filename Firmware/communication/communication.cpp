
/* Includes ------------------------------------------------------------------*/

#include "communication.h"

#include "interface_usb.h"
#include "interface_uart.h"

#include "odrive_main.h"
#include "protocol.hpp"
#include "freertos_vars.h"
#include "utils.h"

#include "../build/version.h" // autogenerated based on Git state

#include <cmsis_os.h>
#include <memory>
//#include <usbd_cdc_if.h>
//#include <usb_device.h>
//#include <usart.h>
//#include <gpio.h>

#include <type_traits>

/* Private defines -----------------------------------------------------------*/
/* Private macros ------------------------------------------------------------*/
/* Private typedef -----------------------------------------------------------*/
/* Global constant data ------------------------------------------------------*/
/* Global variables ----------------------------------------------------------*/

uint64_t serial_number;
char serial_number_str[13]; // 12 digits + null termination

/* Private constant data -----------------------------------------------------*/
/* Private variables ---------------------------------------------------------*/

#if HW_VERSION_MAJOR == 3
// Determine start address of the OTP struct:
// The OTP is organized into 16-byte blocks.
// If the first block starts with "0xfe" we use the first block.
// If the first block starts with "0x00" and the second block starts with "0xfe",
// we use the second block. This gives the user the chance to screw up once.
// If none of the above is the case, we consider the OTP invalid (otp_ptr will be NULL).
const uint8_t* otp_ptr =
    (*(uint8_t*)FLASH_OTP_BASE == 0xfe) ? (uint8_t*)FLASH_OTP_BASE :
    (*(uint8_t*)FLASH_OTP_BASE != 0x00) ? NULL :
        (*(uint8_t*)(FLASH_OTP_BASE + 0x10) != 0xfe) ? NULL :
            (uint8_t*)(FLASH_OTP_BASE + 0x10);

// Read hardware version from OTP if available, otherwise fall back
// to software defined version.
const uint8_t hw_version_major = otp_ptr ? otp_ptr[3] : HW_VERSION_MAJOR;
const uint8_t hw_version_minor = otp_ptr ? otp_ptr[4] : HW_VERSION_MINOR;
const uint8_t hw_version_variant = otp_ptr ? otp_ptr[5] : HW_VERSION_VOLTAGE;
#else
#error "not implemented"
#endif

// the corresponding macros are defined in the autogenerated version.h
const uint8_t fw_version_major = FW_VERSION_MAJOR;
const uint8_t fw_version_minor = FW_VERSION_MINOR;
const uint8_t fw_version_revision = FW_VERSION_REVISION;
const uint8_t fw_version_unreleased = FW_VERSION_UNRELEASED; // 0 for official releases, 1 otherwise

osThreadId comm_thread;

/* Private function prototypes -----------------------------------------------*/
/* Function implementations --------------------------------------------------*/

void init_communication(void) {
    printf("hi!\r\n");

    // Start command handling thread
    osThreadDef(task_cmd_parse, communication_task, osPriorityNormal, 0, 5000 /* in 32-bit words */); // TODO: fix stack issues
    comm_thread = osThreadCreate(osThread(task_cmd_parse), NULL);
}


float oscilloscope[OSCILLOSCOPE_SIZE] = {
    0.123f, 0.345f, 0.4576f, 1.543f, -50.0f
};
size_t oscilloscope_pos = 0;


// Helper class because the protocol library doesn't yet
// support non-member functions
// TODO: make this go away
class StaticFunctions {
public:
    void save_configuration_helper() { save_configuration(); }
    void erase_configuration_helper() { erase_configuration(); }
    void NVIC_SystemReset_helper() { NVIC_SystemReset(); }
    void enter_dfu_mode_helper() { enter_dfu_mode(); }
    float get_oscilloscope_val(uint32_t index) { return oscilloscope[index]; }
    int32_t test_function(int32_t delta) { static int cnt = 0; return cnt += delta; }
} static_functions;

// When adding new functions/variables to the protocol, be careful not to
// blow the communication stack. You can check comm_stack_info to see
// how much headroom you have.
static inline auto make_obj_tree() {
    return make_protocol_member_list(
        make_protocol_ro_property("vbus_voltage", &vbus_voltage),
        make_protocol_ro_property("serial_number", &serial_number),
        make_protocol_ro_property("hw_version_major", &hw_version_major),
        make_protocol_ro_property("hw_version_minor", &hw_version_minor),
        make_protocol_ro_property("hw_version_variant", &hw_version_variant),
        make_protocol_ro_property("fw_version_major", &fw_version_major),
        make_protocol_ro_property("fw_version_minor", &fw_version_minor),
        make_protocol_ro_property("fw_version_revision", &fw_version_revision),
        make_protocol_ro_property("fw_version_unreleased", &fw_version_unreleased),
        make_protocol_ro_property("user_config_loaded", const_cast<const bool *>(&user_config_loaded_)),
        make_protocol_ro_property("brake_resistor_armed", &brake_resistor_armed_),
        make_protocol_object("system_stats",
            make_protocol_ro_property("uptime", &system_stats_.uptime),
            make_protocol_ro_property("min_heap_space", &system_stats_.min_heap_space),
            make_protocol_ro_property("min_stack_space_axis0", &system_stats_.min_stack_space_axis0),
            make_protocol_ro_property("min_stack_space_axis1", &system_stats_.min_stack_space_axis1),
            make_protocol_ro_property("min_stack_space_comms", &system_stats_.min_stack_space_comms),
            make_protocol_ro_property("min_stack_space_usb", &system_stats_.min_stack_space_usb),
            make_protocol_ro_property("min_stack_space_uart", &system_stats_.min_stack_space_uart),
            make_protocol_ro_property("min_stack_space_usb_irq", &system_stats_.min_stack_space_usb_irq),
            make_protocol_ro_property("min_stack_space_startup", &system_stats_.min_stack_space_startup),
            make_protocol_object("usb",
                make_protocol_ro_property("rx_cnt", &usb_stats_.rx_cnt),
                make_protocol_ro_property("tx_cnt", &usb_stats_.tx_cnt),
                make_protocol_ro_property("tx_overrun_cnt", &usb_stats_.tx_overrun_cnt)
            )
        ),
        make_protocol_object("config",
            make_protocol_property("brake_resistance", &board_config.brake_resistance),
            // TODO: changing this currently requires a reboot - fix this
            make_protocol_property("enable_uart", &board_config.enable_uart),
            make_protocol_property("dc_bus_undervoltage_trip_level", &board_config.dc_bus_undervoltage_trip_level),
            make_protocol_property("dc_bus_overvoltage_trip_level", &board_config.dc_bus_overvoltage_trip_level)
        ),
        make_protocol_object("axis0", axes[0]->make_protocol_definitions()),
        make_protocol_object("axis1", axes[1]->make_protocol_definitions()),
        make_protocol_function("get_oscilloscope_val", static_functions, &StaticFunctions::get_oscilloscope_val, "index"),
        make_protocol_function("test_function", static_functions, &StaticFunctions::test_function, "delta"),
        make_protocol_function("save_configuration", static_functions, &StaticFunctions::save_configuration_helper),
        make_protocol_function("erase_configuration", static_functions, &StaticFunctions::erase_configuration_helper),
        make_protocol_function("reboot", static_functions, &StaticFunctions::NVIC_SystemReset_helper),
        make_protocol_function("enter_dfu_mode", static_functions, &StaticFunctions::enter_dfu_mode_helper)
    );
}

using tree_type = decltype(make_obj_tree());
uint8_t tree_buffer[sizeof(tree_type)];

// the protocol has one additional built-in endpoint
constexpr size_t MAX_ENDPOINTS = decltype(make_obj_tree())::endpoint_count + 1;
Endpoint* endpoints_[MAX_ENDPOINTS] = { 0 };
const size_t max_endpoints_ = MAX_ENDPOINTS;
size_t n_endpoints_ = 0;

// Thread to handle deffered processing of USB interrupt, and
// read commands out of the UART DMA circular buffer
void communication_task(void * ctx) {
    (void) ctx; // unused parameter

    // TODO: this is supposed to use the move constructor, but currently
    // the compiler uses the copy-constructor instead. Thus the make_obj_tree
    // ends up with a stupid stack size of around 8000 bytes. Fix this.
    auto tree_ptr = new (tree_buffer) tree_type(make_obj_tree());
    auto endpoint_provider = EndpointProvider_from_MemberList<tree_type>(*tree_ptr);
    set_application_endpoints(&endpoint_provider);
    
    serve_on_uart();
    serve_on_usb();

    for (;;) {
        osDelay(1000); // nothing to do
    }
}

extern "C" {
int _write(int file, const char* data, int len);
}

// @brief This is what printf calls internally
int _write(int file, const char* data, int len) {
#ifdef USB_PROTOCOL_STDOUT
    usb_stream_output.process_bytes((const uint8_t *)data, len);
#endif
#ifdef UART_PROTOCOL_STDOUT
    uart4_stream_output.process_bytes((const uint8_t *)data, len);
#endif
    return len;
}
