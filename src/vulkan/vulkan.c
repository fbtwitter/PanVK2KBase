#include <stdio.h>
#include <stdlib.h>
#include <vulkan/vulkan.h>

int main()
{
    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "kbase-test",
        .applicationVersion = 1,
        .pEngineName = "none",
        .engineVersion = 1,
        .apiVersion = VK_API_VERSION_1_1
    };

    VkInstanceCreateInfo instance_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app
    };

    VkInstance instance;

    VkResult res = vkCreateInstance(
        &instance_info,
        NULL,
        &instance
    );

    if (res != VK_SUCCESS) {
        printf("vkCreateInstance failed: %d\n", res);
        return 1;
    }

    printf("Vulkan instance created\n");


    uint32_t gpu_count = 0;

    vkEnumeratePhysicalDevices(
        instance,
        &gpu_count,
        NULL
    );

    printf("GPU count: %u\n", gpu_count);

    if (gpu_count == 0) {
        printf("No GPU found\n");
        return 1;
    }


    VkPhysicalDevice *gpus =
        calloc(gpu_count, sizeof(VkPhysicalDevice));

    vkEnumeratePhysicalDevices(
        instance,
        &gpu_count,
        gpus
    );


    VkPhysicalDeviceProperties props;

    vkGetPhysicalDeviceProperties(
        gpus[0],
        &props
    );

    printf("GPU: %s\n", props.deviceName);


    uint32_t queue_count = 0;

    vkGetPhysicalDeviceQueueFamilyProperties(
        gpus[0],
        &queue_count,
        NULL
    );

    VkQueueFamilyProperties *queues =
        calloc(queue_count, sizeof(VkQueueFamilyProperties));

    vkGetPhysicalDeviceQueueFamilyProperties(
        gpus[0],
        &queue_count,
        queues
    );


    uint32_t compute_family = UINT32_MAX;

    for (uint32_t i = 0; i < queue_count; i++) {
        printf("queue %u flags: 0x%x\n",
               i,
               queues[i].queueFlags);

        if (queues[i].queueFlags &
            VK_QUEUE_COMPUTE_BIT)
        {
            compute_family = i;
            break;
        }
    }

    if (compute_family == UINT32_MAX) {
        printf("No compute queue\n");
        return 1;
    }


    float priority = 1.0f;

    VkDeviceQueueCreateInfo queue_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = compute_family,
        .queueCount = 1,
        .pQueuePriorities = &priority
    };


    VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_info
    };


    VkDevice device;

    res = vkCreateDevice(
        gpus[0],
        &device_info,
        NULL,
        &device
    );

    printf("vkCreateDevice result: %d\n", res);

    if (res == VK_SUCCESS) {
        printf("Device created\n");
        vkDestroyDevice(device, NULL);
    }


    vkDestroyInstance(instance, NULL);

    free(gpus);
    free(queues);

    return 0;
}