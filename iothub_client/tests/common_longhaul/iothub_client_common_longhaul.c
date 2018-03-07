// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#include "iothub_client_common_longhaul.h"
#include <stdio.h>
#include <stdbool.h>
#include <time.h>
#include "azure_c_shared_utility/optimize_size.h"
#include "azure_c_shared_utility/xlogging.h"
#include "azure_c_shared_utility/platform.h"
#include "azure_c_shared_utility/uuid.h"
#include "azure_c_shared_utility/threadapi.h"
#include "azure_c_shared_utility/crt_abstractions.h"
#include "iothub_client_options.h"
#include "iothub_client.h"
#include "iothub_message.h"
#include "iothub_service_client_auth.h"
#include "iothub_messaging.h"
#include "iothub_devicemethod.h"
#include "iothub_devicetwin.h"
#include "iothubtransport_amqp_messenger.h"
#include "iothubtest.h"
#include "parson.h"

#define MESSAGE_TEST_ID_FIELD          "longhaul-tests"
#define MESSAGE_ID_FIELD               "message-id"
#define LONGHAUL_DEVICE_METHOD_NAME    "longhaulDeviceMethod"

#ifdef MBED_BUILD_TIMESTAMP
#define SET_TRUSTED_CERT_IN_SAMPLES
#endif // MBED_BUILD_TIMESTAMP

#ifdef SET_TRUSTED_CERT_IN_SAMPLES
#include "certs.h"
#endif // SET_TRUSTED_CERT_IN_SAMPLES

DEFINE_ENUM_STRINGS(IOTHUB_CLIENT_CONNECTION_STATUS, IOTHUB_CLIENT_CONNECTION_STATUS_VALUES)
DEFINE_ENUM_STRINGS(IOTHUB_CLIENT_CONNECTION_STATUS_REASON, IOTHUB_CLIENT_CONNECTION_STATUS_REASON_VALUES)
DEFINE_ENUM_STRINGS(IOTHUB_MESSAGING_RESULT, IOTHUB_MESSAGING_RESULT_VALUES)

#define INDEFINITE_TIME                         ((time_t)-1)
#define SERVICE_EVENT_WAIT_TIME_DELTA_SECONDS   60

#define MAX_TELEMETRY_TRAVEL_TIME_SECS          300.0
#define MAX_C2D_TRAVEL_TIME_SECS                300.0
#define MAX_DEVICE_METHOD_TRAVEL_TIME_SECS      300
#define MAX_TWIN_DESIRED_PROP_TRAVEL_TIME_SECS  300.0
#define MAX_TWIN_REPORTED_PROP_TRAVEL_TIME_SECS 300.0

typedef struct IOTHUB_LONGHAUL_RESOURCES_TAG
{
    char* test_id;
    LOCK_HANDLE lock;
    IOTHUB_ACCOUNT_INFO_HANDLE iotHubAccountInfo;
    IOTHUB_CLIENT_STATISTICS_HANDLE iotHubClientStats;
    IOTHUB_CLIENT_HANDLE iotHubClientHandle;
    IOTHUB_SERVICE_CLIENT_AUTH_HANDLE iotHubServiceClientHandle;
    bool is_svc_cl_c2d_msgr_open;
    IOTHUB_MESSAGING_CLIENT_HANDLE iotHubSvcMsgHandle;
    IOTHUB_SERVICE_CLIENT_DEVICE_METHOD_HANDLE iotHubSvcDevMethodHandle;
    IOTHUB_TEST_HANDLE iotHubTestHandle;
    IOTHUB_PROVISIONED_DEVICE* deviceInfo;
    unsigned int counter;
} IOTHUB_LONGHAUL_RESOURCES;

typedef struct SEND_TELEMETRY_CONTEXT_TAG
{
    unsigned int message_id;
    IOTHUB_LONGHAUL_RESOURCES* iotHubLonghaul;
} SEND_TELEMETRY_CONTEXT;

typedef struct SEND_C2D_CONTEXT_TAG
{
    unsigned int message_id;
    IOTHUB_LONGHAUL_RESOURCES* iotHubLonghaul;
} SEND_C2D_CONTEXT;

static time_t add_seconds(time_t base_time, int seconds)
{
    time_t new_time;
    struct tm *bd_new_time;

    if ((bd_new_time = localtime(&base_time)) == NULL)
    {
        new_time = INDEFINITE_TIME;
    }
    else
    {
        bd_new_time->tm_sec += seconds;
        new_time = mktime(bd_new_time);
    }

    return new_time;
}

static int parse_message(const char* data, size_t size, char* test_id, unsigned int* message_id)
{
    int result;
    JSON_Value root_value;

    if ((root_value = json_parse_string(data)) == NULL)
    {
        // TODO: continue from here.
    }

    return result;
}

static char* create_message(const char* test_id, unsigned int message_id)
{
    char* result;
    JSON_Value* root_value;

    if ((root_value = json_value_init_object()) == NULL)
    {
        LogError("Failed creating root json value");
        result = NULL;
    }
    else
    {
        JSON_Object* root_object;

        if ((root_object = json_value_get_object(root_value)) == NULL)
        {
            LogError("Failed creating root json object");
            result = NULL;
        }
        else
        {
            if (json_object_set_string(root_object, MESSAGE_TEST_ID_FIELD, test_id) != JSONSuccess)
            {
                LogError("Failed setting test id");
                result = NULL;
            }
            else if (json_object_set_number(root_object, MESSAGE_ID_FIELD, (double)message_id) != JSONSuccess)
            {
                LogError("Failed setting message id");
                result = NULL;
            }
            else if ((result = json_serialize_to_string(root_value)) == NULL)
            {
                LogError("Failed serializing json to string");
            }

            if (json_object_clear(root_object) != JSONSuccess)
            {
                LogError("Failed clearing root object");
            }
        }

        json_value_free(root_value);
    }

    return result;
}

static IOTHUB_MESSAGE_HANDLE create_iothub_message(const char* test_id, unsigned int message_id)
{
    IOTHUB_MESSAGE_HANDLE result;
    char* msg_text;

    if ((msg_text = create_message(test_id, message_id)) == NULL)
    {
        LogError("Failed creating text for iothub message");
        result = NULL;
    }
    else
    {
        if ((result = IoTHubMessage_CreateFromString(msg_text)) == NULL)
        {
            LogError("Failed creating IOTHUB_MESSAGE_HANDLE");
        }

        free(msg_text);
    }

    return result;
}

static void connection_status_callback(IOTHUB_CLIENT_CONNECTION_STATUS status, IOTHUB_CLIENT_CONNECTION_STATUS_REASON reason, void* userContextCallback)
{
    IOTHUB_LONGHAUL_RESOURCES* iotHubLonghaul = (IOTHUB_LONGHAUL_RESOURCES*)userContextCallback;

    if (iothub_client_statistics_add_connection_status(iotHubLonghaul->iotHubClientStats, status, reason) != 0)
    {
        LogError("Failed adding connection status statistics (%s, %s)", 
            ENUM_TO_STRING(IOTHUB_CLIENT_CONNECTION_STATUS, status), ENUM_TO_STRING(IOTHUB_CLIENT_CONNECTION_STATUS_REASON, reason));
    }
}

static IOTHUBMESSAGE_DISPOSITION_RESULT on_c2d_message_received(IOTHUB_MESSAGE_HANDLE message, void* userContextCallback)
{
    IOTHUBMESSAGE_DISPOSITION_RESULT result;

    if (message == NULL || userContextCallback == 0)
    {
        LogError("Invalid argument (message=%p, userContextCallback=%d)", message, userContextCallback);
        result = IOTHUBMESSAGE_ABANDONED;
    }
    else
    {
        unsigned char* data;
        size_t size;

        if (IoTHubMessage_GetByteArray(message, &data, &size) != IOTHUB_MESSAGE_OK)
        {
            LogError("Failed getting string out of IOTHUB_MESSAGE_HANDLE");
            result = IOTHUBMESSAGE_ABANDONED;
        }
        else
        {
            IOTHUB_LONGHAUL_RESOURCES* iotHubLonghaul = (IOTHUB_LONGHAUL_RESOURCES*)userContextCallback;
            unsigned int message_id;
            char tests_id[40];

            if (parse_message((const char*)data, size, tests_id, &message_id) == 0 &&
                strcmp(tests_id, iotHubLonghaul->test_id) == 0)
            {
                C2D_MESSAGE_INFO info;
                info.message_id = message_id;
                info.time_received = time(NULL);

                if (info.time_received == INDEFINITE_TIME)
                {
                    LogError("Failed setting the receive time for c2d message %d", info.message_id);
                }

                if (iothub_client_statistics_add_c2d_info(iotHubLonghaul->iotHubClientStats, C2D_RECEIVED, &info) != 0)
                {
                    LogError("Failed adding receive info for c2d message %d", info.message_id);
                }

                result = IOTHUBMESSAGE_ACCEPTED;
            }
            else
            {
                result = IOTHUBMESSAGE_ABANDONED;
            }
        }
    }

    return IOTHUBMESSAGE_ACCEPTED;
}

static int on_device_method_received(const char* method_name, const unsigned char* payload, size_t size, unsigned char** response, size_t* response_size, void* userContextCallback)
{
    int result;

    if (method_name == NULL || payload == NULL || size == 0 || response == NULL || response_size == NULL || userContextCallback == NULL)
    {
        LogError("Invalid argument (method_name=%p, payload=%p, size=%zu, response=%p, response_size=%p, userContextCallback=%p)", 
            method_name, payload, size, response, response_size, userContextCallback);
        result = __FAILURE__;
    }
    else if (strcmp(method_name, LONGHAUL_DEVICE_METHOD_NAME) != 0)
    {
        LogError("Unexpected device method received (%s)", method_name);
        result = __FAILURE__;
    }
    else
    {
        IOTHUB_LONGHAUL_RESOURCES* iotHubLonghaul = (IOTHUB_LONGHAUL_RESOURCES*)userContextCallback;
        unsigned int method_id;
        char tests_id[40];

        if (parse_message((const char*)payload, size, tests_id, &method_id) == 0 &&
            strcmp(tests_id, iotHubLonghaul->test_id) == 0)
        {
            DEVICE_METHOD_INFO info;
            info.method_id = method_id;
            info.time_received = time(NULL);

            if (info.time_received == INDEFINITE_TIME)
            {
                LogError("Failed setting the receive time for method %d", info.method_id);
            }

            if (iothub_client_statistics_add_device_method_info(iotHubLonghaul->iotHubClientStats, DEVICE_METHOD_RECEIVED, &info) != 0)
            {
                LogError("Failed adding receive info for method %d", info.method_id);
                result = __FAILURE__;
            }
            else
            {
                result = 0;
            }

            if (mallocAndStrcpy_s((char**)response, (const char*)payload) != 0)
            {
                LogError("Failed setting device method response");
                *response_size = 0;
            }
            else
            {
                *response_size = strlen((const char*)*response);
            }
        }
        else
        {
            result = __FAILURE__; // This is not the message we expected. Abandoning it.
        }
    }

    return result;
}


static unsigned int generate_unique_id(IOTHUB_LONGHAUL_RESOURCES* iotHubLonghaul)
{
    unsigned int result;

    if (Lock(iotHubLonghaul->lock) != LOCK_OK)
    {
        LogError("Failed to lock");
        result = 0;
    }
    else
    {
        result = (++iotHubLonghaul->counter); // Increment first then assign.

        if (Unlock(iotHubLonghaul->lock) != LOCK_OK)
        {
            LogError("Failed to unlock (%d)", result);
        }
    }

    return result;
}

typedef int(*RUN_ON_LOOP_ACTION)(const void* context);

static int run_on_loop(RUN_ON_LOOP_ACTION action, size_t iterationDurationInSeconds, size_t totalDurationInSeconds, const void* action_context)
{
    int result;
    time_t start_time;

    if ((start_time = time(NULL)) == INDEFINITE_TIME)
    {
        LogError("Failed setting start time");
        result = __FAILURE__;
    }
    else
    {
        time_t current_time;
        time_t iteration_start_time;

        result = 0;

        do
        {
            if ((iteration_start_time = time(NULL)) == INDEFINITE_TIME)
            {
                LogError("Failed setting iteration start time");
                result = __FAILURE__;
                break;
            }
            else if (action(action_context) != 0)
            {
                LogError("Loop terminated by action function result");
                result = __FAILURE__;
                break;
            }
            else if ((current_time = time(NULL)) == INDEFINITE_TIME)
            {
                LogError("Failed getting current time");
                result = __FAILURE__;
                break;
            }
            else
            {
                double wait_time_secs = iterationDurationInSeconds - difftime(current_time, iteration_start_time);

                if (wait_time_secs > 0)
                {
                    ThreadAPI_Sleep((unsigned int)(1000 * wait_time_secs));
                }

                // We should get the current time again to be 100% precise, but we will optimize here since wait_time_secs is supposed to be way smaller than totalDurationInSeconds.
            }
        } while (difftime(current_time, start_time) < totalDurationInSeconds);
    }

    return result;
}


typedef enum FUNCTION_RESULT_TAG
{
    FUNCTION_RESULT_SUCCESS,
    FUNCTION_RESULT_FAILURE,
    FUNCTION_RESULT_CONTINUE
} FUNCTION_RESULT;

typedef FUNCTION_RESULT(*AWAITABLE_FUNCTION)(const void* context);

static int wait_for(AWAITABLE_FUNCTION function, size_t maxWaitTimeInSeconds, const void* function_context)
{
    int result;
    time_t start_time;

    if ((start_time = time(NULL)) == INDEFINITE_TIME)
    {
        LogError("Failed setting start time");
        result = __FAILURE__;
    }
    else
    {
        time_t current_time;

        do
        {
            FUNCTION_RESULT func_res = function(function_context);

            if (func_res == FUNCTION_RESULT_SUCCESS)
            {
                result = 0;
                break;
            }
            else if (func_res == FUNCTION_RESULT_FAILURE)
            {
                result = __FAILURE__;
                break;
            }
            else if ((current_time = time(NULL)) == INDEFINITE_TIME)
            {
                LogError("Failed getting current time");
                result = __FAILURE__;
                break;
            }
            else if (difftime(current_time, start_time) >= maxWaitTimeInSeconds)
            {
                LogError("Function timed out");
                result = __FAILURE__;
                break;
            }
            else
            {
                ThreadAPI_Sleep(100);
            }
        } while (true);
    }

    return result;
}


// Public APIs

IOTHUB_ACCOUNT_INFO_HANDLE longhaul_get_account_info(IOTHUB_LONGHAUL_RESOURCES_HANDLE handle)
{
    IOTHUB_ACCOUNT_INFO_HANDLE result;

    if (handle == NULL)
    {
        LogError("Invalid argument (handle is NULL)");
        result = NULL;
    }
    else
    {
        IOTHUB_LONGHAUL_RESOURCES* iotHubLonghaulRsrcs = (IOTHUB_LONGHAUL_RESOURCES*)handle;
        result = iotHubLonghaulRsrcs->iotHubAccountInfo;
    }

    return result;
}

IOTHUB_CLIENT_HANDLE longhaul_get_iothub_client_handle(IOTHUB_LONGHAUL_RESOURCES_HANDLE handle)
{
    IOTHUB_CLIENT_HANDLE result;

    if (handle == NULL)
    {
        LogError("Invalid argument (handle is NULL)");
        result = NULL;
    }
    else
    {
        IOTHUB_LONGHAUL_RESOURCES* iotHubLonghaulRsrcs = (IOTHUB_LONGHAUL_RESOURCES*)handle;
        result = iotHubLonghaulRsrcs->iotHubClientHandle;
    }

    return result;
}

IOTHUB_CLIENT_STATISTICS_HANDLE longhaul_get_statistics(IOTHUB_LONGHAUL_RESOURCES_HANDLE handle)
{
    IOTHUB_CLIENT_STATISTICS_HANDLE result;

    if (handle == NULL)
    {
        LogError("Invalid argument (handle is NULL)");
        result = NULL;
    }
    else
    {
        IOTHUB_LONGHAUL_RESOURCES* iotHubLonghaulRsrcs = (IOTHUB_LONGHAUL_RESOURCES*)handle;
        result = iotHubLonghaulRsrcs->iotHubClientStats;
    }

    return result;
}

void longhaul_tests_deinit(IOTHUB_LONGHAUL_RESOURCES_HANDLE handle)
{
    if (handle == NULL)
    {
        LogError("Invalid argument (handle is NULL)");
    }
    else
    {
        IOTHUB_LONGHAUL_RESOURCES* iotHubLonghaulRsrcs = (IOTHUB_LONGHAUL_RESOURCES*)handle;

        if (iotHubLonghaulRsrcs->iotHubSvcMsgHandle != NULL)
        {
            IoTHubMessaging_Close(iotHubLonghaulRsrcs->iotHubSvcMsgHandle);
            IoTHubMessaging_Destroy(iotHubLonghaulRsrcs->iotHubSvcMsgHandle);
        }

        if (iotHubLonghaulRsrcs->iotHubSvcDevMethodHandle != NULL)
        {
            IoTHubDeviceMethod_Destroy(iotHubLonghaulRsrcs->iotHubSvcDevMethodHandle);
        }

        if (iotHubLonghaulRsrcs->iotHubServiceClientHandle != NULL)
        {
            IoTHubServiceClientAuth_Destroy(iotHubLonghaulRsrcs->iotHubServiceClientHandle);
        }

        if (iotHubLonghaulRsrcs->iotHubClientHandle != NULL)
        {
            IoTHubClient_Destroy(iotHubLonghaulRsrcs->iotHubClientHandle);
        }

        if (iotHubLonghaulRsrcs->iotHubAccountInfo != NULL)
        {
            IoTHubAccount_deinit(iotHubLonghaulRsrcs->iotHubAccountInfo);
        }

        if (iotHubLonghaulRsrcs->iotHubClientStats != NULL)
        {
            iothub_client_statistics_destroy(iotHubLonghaulRsrcs->iotHubClientStats);
        }

        if (iotHubLonghaulRsrcs->test_id != NULL)
        {
            free(iotHubLonghaulRsrcs->test_id);
        }

        if (iotHubLonghaulRsrcs->lock != NULL)
        {
            Lock_Deinit(iotHubLonghaulRsrcs->lock);
        }

        platform_deinit();

        free((void*)handle);
    }
}

IOTHUB_LONGHAUL_RESOURCES_HANDLE longhaul_tests_init()
{
    IOTHUB_LONGHAUL_RESOURCES* result;
    UUID uuid;

    if (UUID_generate(&uuid) != 0)
    {
        LogError("Failed to generate test ID number");
        result = NULL;
    }
    else
    {
        if ((result = (IOTHUB_LONGHAUL_RESOURCES*)malloc(sizeof(IOTHUB_LONGHAUL_RESOURCES))) == NULL)
        {
            LogError("Failed to allocate IOTHUB_LONGHAUL_RESOURCES struct");
        }
        else
        {
            (void)memset(result, 0, sizeof(IOTHUB_LONGHAUL_RESOURCES));

            if ((result->test_id = UUID_to_string(&uuid)) == NULL)
            {
                LogError("Failed to set test ID number");
                longhaul_tests_deinit(result);
                result = NULL;
            }
            else if (platform_init() != 0)
            {
                LogError("Platform init failed");
                longhaul_tests_deinit(result);
                result = NULL;
            }
            else if ((result->lock = Lock_Init()) == NULL)
            {
                LogError("Failed creating lock");
                longhaul_tests_deinit(result);
                result = NULL;
            }
            else if ((result->iotHubAccountInfo = IoTHubAccount_Init()) == NULL)
            {
                LogError("Failed initializing accounts");
                longhaul_tests_deinit(result);
                result = NULL;
            }
            else if ((result->iotHubClientStats = iothub_client_statistics_create()) == NULL)
            {
                LogError("Failed initializing statistics");
                longhaul_tests_deinit(result);
                result = NULL;
            }
            else
            {
                platform_init();
            }
        }
    }

    return result;
}

IOTHUB_CLIENT_HANDLE longhaul_create_and_connect_device_client(IOTHUB_LONGHAUL_RESOURCES_HANDLE handle, IOTHUB_PROVISIONED_DEVICE* deviceToUse, IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol)
{
    IOTHUB_CLIENT_HANDLE result;

    if (handle == NULL || deviceToUse == NULL)
    {
        LogError("Invalid argument (handle=%p, deviceToUse=%p)", handle, deviceToUse);
        result = NULL;
    }
    else if ((result = IoTHubClient_CreateFromConnectionString(deviceToUse->connectionString, protocol)) == NULL)
    {
        LogError("Could not create IoTHubClient");
    }
    else if (deviceToUse->howToCreate == IOTHUB_ACCOUNT_AUTH_X509 &&
        (IoTHubClient_SetOption(result, OPTION_X509_CERT, deviceToUse->certificate) != IOTHUB_CLIENT_OK ||
            IoTHubClient_SetOption(result, OPTION_X509_PRIVATE_KEY, deviceToUse->primaryAuthentication) != IOTHUB_CLIENT_OK))
    {
        LogError("Could not set the device x509 certificate or privateKey");
        IoTHubClient_Destroy(result);
        result = NULL;
    }
    else
    {
        bool trace = false;

        IOTHUB_LONGHAUL_RESOURCES* iotHubLonghaulRsrcs = (IOTHUB_LONGHAUL_RESOURCES*)handle;
        iotHubLonghaulRsrcs->iotHubClientHandle = result;

#ifdef SET_TRUSTED_CERT_IN_SAMPLES
        (void)IoTHubClient_SetOption(result, OPTION_TRUSTED_CERT, certificates);
#endif
        (void)IoTHubClient_SetOption(result, OPTION_LOG_TRACE, &trace);
        (void)IoTHubClient_SetOption(result, OPTION_PRODUCT_INFO, "C-SDK-LongHaul");

        if (IoTHubClient_SetConnectionStatusCallback(result, connection_status_callback, handle) != IOTHUB_CLIENT_OK)
        {
            LogError("Failed setting the connection status callback");
            IoTHubClient_Destroy(result);
            iotHubLonghaulRsrcs->iotHubClientHandle = NULL;
            result = NULL;
        }
        else if (IoTHubClient_SetMessageCallback(result, on_c2d_message_received, handle) != IOTHUB_CLIENT_OK)
        {
            LogError("Failed to set the cloud-to-device message callback");
            IoTHubClient_Destroy(result);
            iotHubLonghaulRsrcs->iotHubClientHandle = NULL;
            result = NULL;
        }
        else if (IoTHubClient_SetDeviceMethodCallback(result, on_device_method_received, handle) != IOTHUB_CLIENT_OK)
        {
            LogError("Failed to set the device method callback");
            IoTHubClient_Destroy(result);
            iotHubLonghaulRsrcs->iotHubClientHandle = NULL;
            result = NULL;
        }
        else
        {
            iotHubLonghaulRsrcs->deviceInfo = deviceToUse;
        }
    }

    return result;
}

static int on_message_received(void* context, const char* data, size_t size)
{
    int result;

    if (data == NULL || size == 0)
    {
        LogError("Invalid message received (data=%p, size=%d)", data, size);
        result = __FAILURE__;
    }
    else
    {
        IOTHUB_LONGHAUL_RESOURCES* iotHubLonghaul = (IOTHUB_LONGHAUL_RESOURCES*)context;
        unsigned int message_id;
        char tests_id[40];

        if (parse_message(data, size, tests_id, &message_id) == 0 &&
            strcmp(tests_id, iotHubLonghaul->test_id) == 0)
        {
            TELEMETRY_INFO info;
            info.message_id = message_id;
            info.time_received = time(NULL);

            if (info.time_received == INDEFINITE_TIME)
            {
                LogError("Failed setting the receive time for message %d", info.message_id);
            }

            if (iothub_client_statistics_add_telemetry_info(iotHubLonghaul->iotHubClientStats, TELEMETRY_RECEIVED, &info) != 0)
            {
                LogError("Failed adding receive info for message %d", info.message_id);
                result = __FAILURE__;
            }
            else
            {
                result = 0;
            }
        }
        else
        {
            result = __FAILURE__; // This is not the message we expected. Abandoning it.
        }
    }

    return result;
}

int longhaul_start_listening_for_telemetry_messages(IOTHUB_LONGHAUL_RESOURCES_HANDLE handle, IOTHUB_PROVISIONED_DEVICE* deviceToUse)
{
    int result;

    if (handle == NULL)
    {
        LogError("Invalid argument (handle is NULL)");
        result = __FAILURE__;
    }
    else
    {
        IOTHUB_LONGHAUL_RESOURCES* iotHubLonghaul = (IOTHUB_LONGHAUL_RESOURCES*)handle;

        if (iotHubLonghaul->iotHubTestHandle != NULL)
        {
            LogError("IoTHubTest already initialized");
            result = __FAILURE__;
        }
        else
        {
            if ((iotHubLonghaul->iotHubTestHandle = IoTHubTest_Initialize(
                IoTHubAccount_GetEventHubConnectionString(iotHubLonghaul->iotHubAccountInfo),
                IoTHubAccount_GetIoTHubConnString(iotHubLonghaul->iotHubAccountInfo),
                deviceToUse->deviceId, IoTHubAccount_GetEventhubListenName(iotHubLonghaul->iotHubAccountInfo),
                IoTHubAccount_GetEventhubAccessKey(iotHubLonghaul->iotHubAccountInfo),
                IoTHubAccount_GetSharedAccessSignature(iotHubLonghaul->iotHubAccountInfo),
                IoTHubAccount_GetEventhubConsumerGroup(iotHubLonghaul->iotHubAccountInfo))) == NULL)
            {
                LogError("Failed initializing IoTHubTest");
                result = __FAILURE__;
            }
            else
            {
                time_t time_start_range = add_seconds(time(NULL), SERVICE_EVENT_WAIT_TIME_DELTA_SECONDS);

                if (time_start_range == INDEFINITE_TIME)
                {
                    LogError("Could not define the time start range");
                    IoTHubTest_Deinit(iotHubLonghaul->iotHubTestHandle);
                    iotHubLonghaul->iotHubTestHandle = NULL;
                    result = __FAILURE__;
                }
                else if (
                    IoTHubTest_ListenForEventAsync(
                        iotHubLonghaul->iotHubTestHandle,
                        IoTHubAccount_GetIoTHubPartitionCount(iotHubLonghaul->iotHubAccountInfo),
                        time_start_range, on_message_received, iotHubLonghaul) != IOTHUB_TEST_CLIENT_OK)
                {
                    LogError("Failed listening for device to cloud messages");
                    IoTHubTest_Deinit(iotHubLonghaul->iotHubTestHandle);
                    iotHubLonghaul->iotHubTestHandle = NULL;
                    result = __FAILURE__;
                }
                else
                {
                    result = 0;
                }
            }
        }
    }

    return result;
}

int longhaul_stop_listening_for_telemetry_messages(IOTHUB_LONGHAUL_RESOURCES_HANDLE handle)
{
    int result;

    if (handle == NULL)
    {
        LogError("Invalid argument (handle is NULL)");
        result = __FAILURE__;
    }
    else
    {
        IOTHUB_LONGHAUL_RESOURCES* iotHubLonghaul = (IOTHUB_LONGHAUL_RESOURCES*)handle;

        if (iotHubLonghaul->iotHubTestHandle == NULL)
        {
            LogError("IoTHubTest not initialized");
            result = __FAILURE__;
        }
        else
        {
            if (IoTHubTest_ListenForEventAsync(iotHubLonghaul->iotHubTestHandle, 0, INDEFINITE_TIME, NULL, NULL) != IOTHUB_TEST_CLIENT_OK)
            {
                LogError("Failed stopping listening for device to cloud messages");
            }

            IoTHubTest_Deinit(iotHubLonghaul->iotHubTestHandle);
            iotHubLonghaul->iotHubTestHandle = NULL;

            result = 0;
        }
    }

    return result;
}

static IOTHUB_SERVICE_CLIENT_AUTH_HANDLE longhaul_initialize_service_client(IOTHUB_LONGHAUL_RESOURCES* iotHubLonghaul)
{
    if (iotHubLonghaul->iotHubServiceClientHandle == NULL)
    {
        const char* connection_string;

        if ((connection_string = IoTHubAccount_GetIoTHubConnString(iotHubLonghaul->iotHubAccountInfo)) == NULL)
        {
            LogError("Failed retrieving the IoT hub connection string");
        }
        else 
        { 
            iotHubLonghaul->iotHubServiceClientHandle = IoTHubServiceClientAuth_CreateFromConnectionString(connection_string);
        }
    }

    return iotHubLonghaul->iotHubServiceClientHandle;
}

static void on_svc_client_c2d_messaging_open_complete(void* context)
{
    IOTHUB_LONGHAUL_RESOURCES* iotHubLonghaul = (IOTHUB_LONGHAUL_RESOURCES*)context;

    if (Lock(iotHubLonghaul->lock) != LOCK_OK)
    {
        LogError("Failed locking");
    }
    else
    {
        iotHubLonghaul->is_svc_cl_c2d_msgr_open = true;

        if (Unlock(iotHubLonghaul->lock) != LOCK_OK)
        {
            LogError("Failed unlocking");
        }
    }
}

static IOTHUB_MESSAGING_CLIENT_HANDLE longhaul_initialize_service_c2d_messaging_client(IOTHUB_LONGHAUL_RESOURCES* iotHubLonghaul)
{
    IOTHUB_MESSAGING_CLIENT_HANDLE result;

    if (iotHubLonghaul->iotHubSvcMsgHandle != NULL)
    {
        LogError("IoT Hub Service C2D messaging already initialized");
        result = NULL;
    }
    else if ((iotHubLonghaul->iotHubSvcMsgHandle = IoTHubMessaging_Create(iotHubLonghaul->iotHubServiceClientHandle)) == NULL)
    {
        LogError("Failed creating the IoT Hub Service C2D messenger");
        result = NULL;
    }
    else if (IoTHubMessaging_Open(iotHubLonghaul->iotHubSvcMsgHandle, on_svc_client_c2d_messaging_open_complete, iotHubLonghaul) != IOTHUB_MESSAGING_OK)
    {
        LogError("Failed opening the IoT Hub Service C2D messenger");
        IoTHubMessaging_Destroy(iotHubLonghaul->iotHubSvcMsgHandle);
        iotHubLonghaul->iotHubSvcMsgHandle = NULL;
        result = NULL;
    }
    else
    {
        result = iotHubLonghaul->iotHubSvcMsgHandle;
    }

    return result;
}

static IOTHUB_SERVICE_CLIENT_DEVICE_METHOD_HANDLE longhaul_initialize_service_device_method_client(IOTHUB_LONGHAUL_RESOURCES* iotHubLonghaul)
{
    IOTHUB_SERVICE_CLIENT_DEVICE_METHOD_HANDLE result;

    if (iotHubLonghaul->iotHubSvcDevMethodHandle != NULL)
    {
        LogError("IoT Hub Service device method client already initialized");
        result = NULL;
    }
    else if ((iotHubLonghaul->iotHubSvcDevMethodHandle = IoTHubDeviceMethod_Create(iotHubLonghaul->iotHubServiceClientHandle)) == NULL)
    {
        LogError("Failed creating the IoT Hub Service device method client");
        result = NULL;
    }
    else
    {
        result = iotHubLonghaul->iotHubSvcDevMethodHandle;
    }

    return result;
}

// Conveniency *run* functions

static void send_confirmation_callback(IOTHUB_CLIENT_CONFIRMATION_RESULT result, void* userContextCallback)
{
    if (userContextCallback == NULL)
    {
        LogError("Invalid argument (userContextCallback is NULL)");
    }
    else
    {
        SEND_TELEMETRY_CONTEXT* message_info = (SEND_TELEMETRY_CONTEXT*)userContextCallback;

        TELEMETRY_INFO telemetry_info;
        telemetry_info.message_id = message_info->message_id;
        telemetry_info.send_callback_result = result;
        telemetry_info.time_sent = time(NULL);

        if (telemetry_info.time_sent == INDEFINITE_TIME)
        {
            LogError("Failed setting the time telemetry was sent");
        }
        else if (iothub_client_statistics_add_telemetry_info(message_info->iotHubLonghaul->iotHubClientStats, TELEMETRY_SENT, &telemetry_info) != 0)
        {
            LogError("Failed adding telemetry statistics info (message_id=%d)", message_info->message_id);
        }

        free(message_info);
    }
}

static SEND_TELEMETRY_CONTEXT* create_iothub_message_context(IOTHUB_LONGHAUL_RESOURCES* iotHubLonghaul)
{
    SEND_TELEMETRY_CONTEXT* result;

    if ((result = (SEND_TELEMETRY_CONTEXT*)malloc(sizeof(SEND_TELEMETRY_CONTEXT))) == NULL)
    {
        LogError("Failed allocating telemetry message context");
    }
    else
    {
        result->message_id = iotHubLonghaul->counter;
        result->iotHubLonghaul = iotHubLonghaul;
    }

    return result;
}

static int send_telemetry(const void* context)
{
    int result;
    IOTHUB_LONGHAUL_RESOURCES* longhaulResources = (IOTHUB_LONGHAUL_RESOURCES*)context;
    unsigned int message_id;

    if ((message_id = generate_unique_id(longhaulResources)) == 0)
    {
        LogError("Failed generating telemetry message id");
        result = __FAILURE__;
    }
    else
    {
        IOTHUB_MESSAGE_HANDLE message;

        if ((message = create_iothub_message(longhaulResources->test_id, message_id)) == NULL)
        {
            LogError("Failed creating telemetry message");
            result = __FAILURE__;
        }
        else
        {
            SEND_TELEMETRY_CONTEXT* message_info;

            if ((message_info = (SEND_TELEMETRY_CONTEXT*)malloc(sizeof(SEND_TELEMETRY_CONTEXT))) == NULL)
            {
                LogError("Failed allocating context for telemetry message");
                result = __FAILURE__;
            }
            else
            {
                TELEMETRY_INFO telemetry_info;
                telemetry_info.message_id = message_id;
                telemetry_info.time_queued = time(NULL);

                message_info->message_id = message_id;
                message_info->iotHubLonghaul = longhaulResources;

                if (IoTHubClient_SendEventAsync(longhaulResources->iotHubClientHandle, message, send_confirmation_callback, message_info) != IOTHUB_CLIENT_OK)
                {
                    LogError("Failed sending telemetry message");
                    free(message_info);
                    result = __FAILURE__;
                }
                else
                {
                    result = 0;
                }

                telemetry_info.send_result = result;

                if (iothub_client_statistics_add_telemetry_info(longhaulResources->iotHubClientStats, TELEMETRY_QUEUED, &telemetry_info) != 0)
                {
                    LogError("Failed adding telemetry statistics info (message_id=%d)", message_id);
                    result = __FAILURE__;
                }
            }

            IoTHubMessage_Destroy(message);
        }
    }

    return result;
}

static void on_c2d_message_sent(void* context, IOTHUB_MESSAGING_RESULT messagingResult)
{
    if (context == NULL)
    {
        LogError("Invalid argument (%p, %s)", context, ENUM_TO_STRING(IOTHUB_MESSAGING_RESULT, messagingResult));
    }
    else
    {
        SEND_C2D_CONTEXT* send_context = (SEND_C2D_CONTEXT*)context;

        C2D_MESSAGE_INFO info;
        info.message_id = send_context->message_id;
        info.send_callback_result = messagingResult;
        info.time_sent = time(NULL);

        if (info.time_sent == INDEFINITE_TIME)
        {
            LogError("Failed setting the send time for message %d", info.message_id);
        }

        if (iothub_client_statistics_add_c2d_info(send_context->iotHubLonghaul->iotHubClientStats, C2D_SENT, &info) != 0)
        {
            LogError("Failed adding send info for c2d message %d", info.message_id);
        }

        free(send_context);
    }
}

static int send_c2d(const void* context)
{
    int result;
    IOTHUB_LONGHAUL_RESOURCES* iotHubLonghaul = (IOTHUB_LONGHAUL_RESOURCES*)context;
    unsigned int message_id;

    if ((message_id = generate_unique_id(iotHubLonghaul)) == 0)
    {
        LogError("Failed generating c2d message id");
        result = __FAILURE__;
    }
    else
    {
        IOTHUB_MESSAGE_HANDLE message;

        if ((message = create_iothub_message(iotHubLonghaul->test_id, message_id)) == NULL)
        {
            LogError("Failed creating C2D message text");
            result = __FAILURE__;
        }
        else
        {
            SEND_C2D_CONTEXT* send_context;

            if ((send_context = (SEND_C2D_CONTEXT*)malloc(sizeof(SEND_C2D_CONTEXT))) == NULL)
            {
                LogError("Failed allocating context for sending c2d message");
                result = __FAILURE__;
            }
            else
            {
                send_context->message_id = message_id;
                send_context->iotHubLonghaul = iotHubLonghaul;

                if (IoTHubMessaging_SendAsync(iotHubLonghaul->iotHubSvcMsgHandle, iotHubLonghaul->deviceInfo->deviceId, message, on_c2d_message_sent, send_context) != IOTHUB_MESSAGING_OK)
                {
                    LogError("Failed sending c2d message");
                    free(send_context);
                    result = __FAILURE__;
                }
                else
                {
                    result = 0;
                }

                {
                    C2D_MESSAGE_INFO c2d_msg_info;
                    c2d_msg_info.message_id = message_id;
                    c2d_msg_info.time_queued = time(NULL);
                    c2d_msg_info.send_result = result;

                    if (iothub_client_statistics_add_c2d_info(iotHubLonghaul->iotHubClientStats, C2D_QUEUED, &c2d_msg_info) != 0)
                    {
                        LogError("Failed adding c2d message statistics info (message_id=%d)", message_id);
                        result = __FAILURE__;
                    }
                }
            }

            IoTHubMessage_Destroy(message);
        }
    }

    return result;
}

static int invoke_device_method(const void* context)
{
    int result;
    IOTHUB_LONGHAUL_RESOURCES* iotHubLonghaul = (IOTHUB_LONGHAUL_RESOURCES*)context;
    unsigned int method_id;

    if ((method_id = generate_unique_id(iotHubLonghaul)) == 0)
    {
        LogError("Failed generating device method id");
        result = __FAILURE__;
    }
    else
    {
        char* message;

        if ((message = create_message(iotHubLonghaul->test_id, method_id)) == NULL)
        {
            LogError("Failed creating C2D message text");
            result = __FAILURE__;
        }
        else
        {
            int responseStatus;
            unsigned char* responsePayload;
            size_t responseSize;

            DEVICE_METHOD_INFO device_method_info;
            device_method_info.method_id = method_id;
            device_method_info.time_invoked = time(NULL);

            if ((device_method_info.method_result = IoTHubDeviceMethod_Invoke(
                iotHubLonghaul->iotHubSvcDevMethodHandle, 
                iotHubLonghaul->deviceInfo->deviceId, 
                LONGHAUL_DEVICE_METHOD_NAME, 
                message, 
                MAX_DEVICE_METHOD_TRAVEL_TIME_SECS, 
                &responseStatus, &responsePayload, &responseSize)) != IOTHUB_DEVICE_METHOD_OK)
            {
                LogError("Failed invoking device method");
            }

            if (iothub_client_statistics_add_device_method_info(iotHubLonghaul->iotHubClientStats, DEVICE_METHOD_INVOKED, &device_method_info) != 0)
            {
                LogError("Failed adding device method statistics info (method_id=%d)", method_id);
                result = __FAILURE__;
            }
            else
            {
                result = 0;
            }

            free(message);
        }
    }

    return result;
}

int longhaul_run_telemetry_tests(IOTHUB_LONGHAUL_RESOURCES_HANDLE handle, size_t iterationDurationInSeconds, size_t totalDurationInSeconds)
{
    int result;

    if (handle == NULL)
    {
        LogError("Invalig argument (handle is NULL)");
        result = __FAILURE__;
    }
    else
    {
        IOTHUB_LONGHAUL_RESOURCES* iotHubLonghaulRsrcs = (IOTHUB_LONGHAUL_RESOURCES*)handle;

        if (iotHubLonghaulRsrcs->iotHubClientHandle == NULL || iotHubLonghaulRsrcs->deviceInfo == NULL)
        {
            LogError("IoTHubClient not initialized.");
            result = __FAILURE__;
        }
        else
        {
            if (longhaul_start_listening_for_telemetry_messages(handle, iotHubLonghaulRsrcs->deviceInfo) != 0)
            {
                LogError("Failed listening for telemetry messages");
                result = __FAILURE__;
            }
            else
            {
                int loop_result;
                IOTHUB_CLIENT_STATISTICS_HANDLE stats_handle;

                loop_result = run_on_loop(send_telemetry, iterationDurationInSeconds, totalDurationInSeconds, iotHubLonghaulRsrcs);

                ThreadAPI_Sleep(iterationDurationInSeconds * 1000 * 10); // Extra time for the last messages.

                stats_handle = longhaul_get_statistics(iotHubLonghaulRsrcs);

                LogInfo("Longhaul telemetry stats: %s", iothub_client_statistics_to_json(stats_handle));

                if (loop_result != 0)
                {
                    result = __FAILURE__;
                }
                else
                {
                    IOTHUB_CLIENT_STATISTICS_TELEMETRY_SUMMARY summary;

                    if (iothub_client_statistics_get_telemetry_summary(stats_handle, &summary) != 0)
                    {
                        LogError("Failed gettting statistics summary");
                        result = __FAILURE__;
                    }
                    else
                    {
                        LogInfo("Summary: Messages sent=%d, received=%d; travel time: min=%f secs, max=%f secs", 
                            summary.messages_sent, summary.messages_received, summary.min_travel_time_secs, summary.max_travel_time_secs);
                     
                        if (summary.messages_sent == 0 || summary.messages_received != summary.messages_sent || summary.max_travel_time_secs > MAX_TELEMETRY_TRAVEL_TIME_SECS)
                        {
                            result = __FAILURE__;
                        }
                        else
                        {
                            result = 0;
                        }
                    }
                }

                (void)longhaul_stop_listening_for_telemetry_messages(iotHubLonghaulRsrcs);
            }
        }
    }

    return result;
}

int longhaul_run_c2d_tests(IOTHUB_LONGHAUL_RESOURCES_HANDLE handle, size_t iterationDurationInSeconds, size_t totalDurationInSeconds)
{
    int result;

    if (handle == NULL)
    {
        LogError("Invalig argument (handle is NULL)");
        result = __FAILURE__;
    }
    else
    {
        IOTHUB_LONGHAUL_RESOURCES* iotHubLonghaul = (IOTHUB_LONGHAUL_RESOURCES*)handle;

        if (iotHubLonghaul->iotHubClientHandle == NULL || iotHubLonghaul->deviceInfo == NULL)
        {
            LogError("IoTHubClient not initialized.");
            result = __FAILURE__;
        }
        else if (longhaul_initialize_service_client(iotHubLonghaul) == NULL)
        {
            LogError("Cannot send C2D messages, failed to initialize IoT hub service client");
            result = __FAILURE__;
        }
        else if (longhaul_initialize_service_c2d_messaging_client(iotHubLonghaul) == NULL)
        {
            LogError("Cannot send C2D messages, failed to initialize IoT hub service client c2d messenger");
            result = __FAILURE__;
        }
        else
        {


            {
                int loop_result;
                IOTHUB_CLIENT_STATISTICS_HANDLE stats_handle;

                loop_result = run_on_loop(send_c2d, iterationDurationInSeconds, totalDurationInSeconds, iotHubLonghaul);

                //ThreadAPI_Sleep(iterationDurationInSeconds * 1000 * 10); // Extra time for the last messages.

                stats_handle = longhaul_get_statistics(iotHubLonghaul);

                LogInfo("Longhaul Cloud-to-Device stats: %s", iothub_client_statistics_to_json(stats_handle));

                if (loop_result != 0)
                {
                    result = __FAILURE__;
                }
                else
                {
                    IOTHUB_CLIENT_STATISTICS_C2D_SUMMARY summary;

                    if (iothub_client_statistics_get_c2d_summary(stats_handle, &summary) != 0)
                    {
                        LogError("Failed gettting statistics summary");
                        result = __FAILURE__;
                    }
                    else
                    {
                        LogInfo("Summary: Messages sent=%d, received=%d; travel time: min=%f secs, max=%f secs",
                            summary.messages_sent, summary.messages_received, summary.min_travel_time_secs, summary.max_travel_time_secs);

                        if (summary.messages_sent == 0 || summary.messages_received != summary.messages_sent || summary.max_travel_time_secs > MAX_C2D_TRAVEL_TIME_SECS)
                        {
                            result = __FAILURE__;
                        }
                        else
                        {
                            result = 0;
                        }
                    }
                }
            }
        }
    }

    return result;
}

int longhaul_run_device_methods_tests(IOTHUB_LONGHAUL_RESOURCES_HANDLE handle, size_t iterationDurationInSeconds, size_t totalDurationInSeconds)
{
    int result;

    if (handle == NULL)
    {
        LogError("Invalig argument (handle is NULL)");
        result = __FAILURE__;
    }
    else
    {
        IOTHUB_LONGHAUL_RESOURCES* iotHubLonghaul = (IOTHUB_LONGHAUL_RESOURCES*)handle;

        if (iotHubLonghaul->iotHubClientHandle == NULL || iotHubLonghaul->deviceInfo == NULL)
        {
            LogError("IoTHubClient not initialized.");
            result = __FAILURE__;
        }
        else if (longhaul_initialize_service_client(iotHubLonghaul) == NULL)
        {
            LogError("Cannot invoke device methods, failed to initialize IoT hub service client");
            result = __FAILURE__;
        }
        else if (longhaul_initialize_service_device_method_client(iotHubLonghaul) == NULL)
        {
            LogError("Cannot invoke device methods, failed to initialize IoT hub service device method client");
            result = __FAILURE__;
        }
        else
        {
            int loop_result;
            IOTHUB_CLIENT_STATISTICS_HANDLE stats_handle;

            loop_result = run_on_loop(invoke_device_method, iterationDurationInSeconds, totalDurationInSeconds, iotHubLonghaul);

            // TODO: uncomment below
            //ThreadAPI_Sleep(iterationDurationInSeconds * 1000 * 10); // Extra time for the last messages.

            stats_handle = longhaul_get_statistics(iotHubLonghaul);

            LogInfo("Longhaul Device Methods stats: %s", iothub_client_statistics_to_json(stats_handle));

            if (loop_result != 0)
            {
                result = __FAILURE__;
            }
            else
            {
                IOTHUB_CLIENT_STATISTICS_DEVICE_METHOD_SUMMARY summary;

                if (iothub_client_statistics_get_device_method_summary(stats_handle, &summary) != 0)
                {
                    LogError("Failed gettting statistics summary");
                    result = __FAILURE__;
                }
                else
                {
                    LogInfo("Summary: Methods invoked=%d, received=%d; travel time: min=%f secs, max=%f secs",
                        summary.methods_invoked, summary.methods_received, summary.min_travel_time_secs, summary.max_travel_time_secs);

                    if (summary.methods_invoked == 0 || summary.methods_received != summary.methods_invoked || summary.max_travel_time_secs > MAX_DEVICE_METHOD_TRAVEL_TIME_SECS)
                    {
                        result = __FAILURE__;
                    }
                    else
                    {
                        result = 0;
                    }
                }
            }
        }
    }

    return result;
}

int longhaul_run_twin_desired_properties_tests(IOTHUB_LONGHAUL_RESOURCES_HANDLE handle, size_t iterationDurationInSeconds, size_t totalDurationInSeconds)
{
    (void)handle;
    (void)iterationDurationInSeconds;
    (void)totalDurationInSeconds;
    // TO BE SENT ON A SEPARATE CODE REVIEW
    return 0;
}

int longhaul_run_twin_reported_properties_tests(IOTHUB_LONGHAUL_RESOURCES_HANDLE handle, size_t iterationDurationInSeconds, size_t totalDurationInSeconds)
{
    (void)handle;
    (void)iterationDurationInSeconds;
    (void)totalDurationInSeconds;
    // TO BE SENT ON A SEPARATE CODE REVIEW
    return 0;
}