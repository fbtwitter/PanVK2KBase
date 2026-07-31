Sending commands to kbase CSF is done through these steps:

- Create a GROUP_QUEUE with KBASE_IOCTL_CS_QUEUE_GROUP_CREATE
- Allocate GPU memory for the queue commands
- Register the queue with KBASE_IOCTL_CS_QUEUE_REGISTER
- Bind the queue with KBASE_IOCTL_CS_QUEUE_BIND. This provides you with a memory address that works as the IO interface of the queue
-- This is formed by 3 pages, input, output and doorbell
- Kick the queue with KBASE_IOCTL_CS_QUEUE_KICK (sets it for execution)
- Ring the doorbell (signals the execution of the code in the queue)