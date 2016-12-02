#include "headers.h"
#ifdef ALTERA
    #include "AOCL_Utils.h"
    using namespace aocl_utils;
#else
    #include "CL/cl.h"
    void checkError(cl_int err, const char *operation){
        if (err != CL_SUCCESS){
            fprintf(stderr, "Error during operation '%s': %d\n", operation, err);
            exit(1);
        }
    }
#endif
#ifdef NVIDIA
    #define VENDOR "NVIDIA Corporation"
#endif
#ifdef IOCL
    #define VENDOR "Intel(R) Corporation"
#endif

cl_platform_id platform = NULL;
cl_device_id device;
cl_context context = NULL;
cl_command_queue queue;
cl_program program = NULL;
cl_kernel kernel;
cl_mem nearest_buf;
cl_mem output_energy_buf;
cl_mem output_force_buf;

cl_float3 position_arr[particles_count] = {};
cl_float3 nearest[particles_count] = {};
cl_float3 velocity[particles_count] = {};

float output_energy[particles_count] = {};
cl_float3 output_force[particles_count] = {};
double kernel_total_time = 0.;

int main() {
    struct timeb start_total_time;
    ftime(&start_total_time);
    if(!init_opencl()) {
      return -1;
    }
    init_problem(position_arr, velocity);
    md(position_arr, nearest, output_force, output_energy, velocity);
    cleanup();
    struct timeb end_total_time;
    ftime(&end_total_time);
    printf("\nTotal execution time in ms =  %d\n", (int)((end_total_time.time - start_total_time.time) * 1000 + end_total_time.millitm - start_total_time.millitm));
    printf("\nKernel execution time in milliseconds = %0.3f ms\n", (kernel_total_time / 1000000.0) );
    printf("\nKernel execution time in milliseconds per iters = %0.3f ms\n", (kernel_total_time / ( total_it * 1000000.0)) );
    return 0;
}

/////// HELPER FUNCTIONS ///////

// Initializes the OpenCL objects.
bool init_opencl() {
    cl_int status;

    printf("Initializing OpenCL\n");
    #ifdef ALTERA
        if(!setCwdToExeDir()) {
          return false;
        }
        platform = findPlatform("Altera");
    #else
        cl_uint num_platforms;
        cl_platform_id pls[MAX_PLATFORMS_COUNT];
        clGetPlatformIDs(MAX_PLATFORMS_COUNT, pls, &num_platforms);
        char vendor[128];
        for (int i = 0; i < MAX_PLATFORMS_COUNT; i++){
            clGetPlatformInfo (pls[i], CL_PLATFORM_VENDOR, sizeof(vendor), vendor, NULL);
            if (!strcmp(VENDOR, vendor))
            {
                platform = pls[i];
                break;
            }
        }
    #endif
    if(platform == NULL) {
      printf("ERROR: Unable to find OpenCL platform.\n");
      return false;
    }

    #ifdef ALTERA
        scoped_array<cl_device_id> devices;
        cl_uint num_devices;
        devices.reset(getDevices(platform, CL_DEVICE_TYPE_ALL, &num_devices));
        // We'll just use the first device.
        device = devices[0];
    #else
        cl_uint num_devices;
        clGetDeviceIDs(platform, CL_DEVICE_TYPE_GPU , 1, &device, &num_devices);
    #endif

    context = clCreateContext(NULL, 1, &device, NULL, NULL, &status);
    checkError(status, "Failed to create context");

    queue = clCreateCommandQueue(context, device, CL_QUEUE_PROFILING_ENABLE, &status);
    checkError(status, "Failed to create command queue");

    #ifdef ALTERA
        std::string binary_file = getBoardBinaryFile("md", device);
        printf("Using AOCX: %s\n", binary_file.c_str());
        program = createProgramFromBinary(context, binary_file.c_str(), &device, 1);
    #else
        int MAX_SOURCE_SIZE  = 65536;
        FILE *fp;
        const char fileName[] = "./device/md.cl";
        size_t source_size;
        char *source_str;
        try {
            fp = fopen(fileName, "r");
            if (!fp) {
                fprintf(stderr, "Failed to load kernel.\n");
                exit(1);
            }
            source_str = (char *)malloc(MAX_SOURCE_SIZE);
            source_size = fread(source_str, 1, MAX_SOURCE_SIZE, fp);
            fclose(fp);
        }
        catch (int a) {
            printf("%f", a);
        }
        program = clCreateProgramWithSource(context, 1, (const char **)&source_str, (const size_t *)&source_size, &status);
    #endif

    status = clBuildProgram(program, 0, NULL, "", NULL, NULL);
    checkError(status, "Failed to build program");

    const char *kernel_name = "md";
    kernel = clCreateKernel(program, kernel_name, &status);
    checkError(status, "Failed to create kernel");

    // Input buffer.
    nearest_buf = clCreateBuffer(context, CL_MEM_READ_ONLY,
        particles_count * sizeof(cl_float3), NULL, &status);
    checkError(status, "Failed to create buffer for nearest");

    // Output buffers.
    output_energy_buf = clCreateBuffer(context, CL_MEM_WRITE_ONLY,
        particles_count * sizeof(float), NULL, &status);
    checkError(status, "Failed to create buffer for output_en");

     output_force_buf = clCreateBuffer(context, CL_MEM_WRITE_ONLY,
        particles_count * sizeof(cl_float3), NULL, &status);
    checkError(status, "Failed to create buffer for output_force");

    return true;
}

void run() {
    cl_int status;

    cl_event kernel_event;
    cl_event finish_event;
    cl_ulong time_start, time_end;
    double total_time;

    cl_event write_event;
    status = clEnqueueWriteBuffer(queue, nearest_buf, CL_FALSE,
        0, particles_count * sizeof(cl_float3), nearest, 0, NULL, &write_event);
    checkError(status, "Failed to transfer nearest");

    unsigned argi = 0;

    size_t global_work_size[1] = {particles_count};
    size_t local_work_size[1] = {particles_count};
    status = clSetKernelArg(kernel, argi++, sizeof(cl_mem), &nearest_buf);
    checkError(status, "Failed to set argument nearest");

    status = clSetKernelArg(kernel, argi++, sizeof(cl_mem), &output_energy_buf);
    checkError(status, "Failed to set argument output_energy");

    status = clSetKernelArg(kernel, argi++, sizeof(cl_mem), &output_force_buf);
    checkError(status, "Failed to set argument output_force");

    status = clEnqueueNDRangeKernel(queue, kernel, 1, NULL,
        global_work_size, local_work_size, 1, &write_event, &kernel_event);
    checkError(status, "Failed to launch kernel");

    status = clEnqueueReadBuffer(queue, output_energy_buf, CL_FALSE,
        0, particles_count * sizeof(float), output_energy, 1, &kernel_event, &finish_event);

    status = clEnqueueReadBuffer(queue, output_force_buf, CL_FALSE,
        0, particles_count * sizeof(cl_float3), output_force, 1, &kernel_event, &finish_event);

    // Release local events.
    clReleaseEvent(write_event);

    // Wait for all devices to finish.
    clWaitForEvents(1, &finish_event);

    clGetEventProfilingInfo(kernel_event, CL_PROFILING_COMMAND_START, sizeof(time_start), &time_start, NULL);
    clGetEventProfilingInfo(kernel_event, CL_PROFILING_COMMAND_END, sizeof(time_end), &time_end, NULL);
    total_time = time_end - time_start;
    kernel_total_time += total_time;

    // Release all events.
    clReleaseEvent(kernel_event);
    clReleaseEvent(finish_event);
}

// Free the resources allocated during initialization
void cleanup() {
    if(kernel) {
      clReleaseKernel(kernel);
    }
    if(queue) {
      clReleaseCommandQueue(queue);
    }
    if(nearest_buf) {
      clReleaseMemObject(nearest_buf);
    }
    if(output_energy_buf) {
      clReleaseMemObject(output_energy_buf);
    }
    if(output_force_buf) {
      clReleaseMemObject(output_force_buf);
    }
    if(program) {
    clReleaseProgram(program);
    }
    if(context) {
    clReleaseContext(context);
    }
}

