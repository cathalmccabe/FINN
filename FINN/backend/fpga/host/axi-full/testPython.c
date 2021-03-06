// #define NPY_NO_DEPRECATED_API NPY_1_7_API_VERSION

#include <string>
#include <iostream>
#include "assert.h"

#include <map>
#include <string>
#include <vector>

#include <boost/python.hpp>
#include <numpy/ndarrayobject.h>
using namespace boost::python;

#include "testPython.h"

#include "foldedmv-offload.h"

#include <sys/time.h>

#ifdef __SDX__
	#include "sdx.hpp"
#endif

#define KERNELDIM 	0
#define STRIDE 		1
#define SPLIT 		2
#define BASEMEM 	3
#define SIMD 		4
#define PE 			5
#define WMEM 		6
#define TMEM 		7
#define BITIN 		8
#define BITOUT		9
#define BITWEIGHTS	10


// Map storing network's layer info
std::map<std::string, std::vector<unsigned int>> networkProperties;


// Simulation datastructures
MemoryWord *inputData, *bufMemIn_ch0, *bufMemIn_ch1;
MemoryWord *outputData;

// Global parameters to handle batch
uint64_t inputMemWords = 0;			// Number of words for 1 input image (padded words)
uint64_t outputMemWords = 0;		// Number of words for 1 output image (padded words)
unsigned int currentBatchSize = 0;	// Images in the current batch
unsigned int maxBatchSize = 0;		// Maximum size of the batch
unsigned int fetchedResults = 0;	// Number of results feched

// Dump all data in the map network parameters
void dumpNetworkParameters(){
	for(auto &v : networkProperties){
		std::cout << v.first << std::endl;
		for(auto &v2 : v.second)
			std::cout << v2 << "\t";
		std::cout << std::endl;
	}
}

// Get the shape of the arr numpy array coming from python
std::vector<unsigned int> getShape(numeric::array & arr){
	std::vector<unsigned int> v;
	tuple shape = extract<tuple>(arr.attr("shape"));
	unsigned int lenght = extract<long>(shape.attr("__len__")());

	for(int i=0; i<lenght; i++)
		v.push_back(extract<long>(shape[i]));

	return v;
}

// Get array size of the numpy array: product of its dimensions
int arraySize(numeric::array & arr)
{
	auto shape = getShape(arr);
	int size = 1;

	for(int i=0; i<shape.size(); i++)
		size *= shape[i];

	return size;
}

// Store informations of convolutional layer in the map
// TODO: The function is used for both convolutional and fully connected layers
//			the name should be changed
void createConvolutionalLayer(std::string layerName, 
	unsigned int kernelDim, unsigned int stride, unsigned int split,
	unsigned int basemem, unsigned int simd, unsigned int pe,
	unsigned int wmem, unsigned int tmem,
	unsigned int bitsIn, unsigned int bitsOut, unsigned int bitWeights){

	networkProperties[layerName].push_back(kernelDim);
	networkProperties[layerName].push_back(stride);
	networkProperties[layerName].push_back(split);
	networkProperties[layerName].push_back(basemem);
	networkProperties[layerName].push_back(simd);
	networkProperties[layerName].push_back(pe);
	networkProperties[layerName].push_back(wmem);
	networkProperties[layerName].push_back(tmem);
	networkProperties[layerName].push_back(bitsIn);
	networkProperties[layerName].push_back(bitsOut);
	networkProperties[layerName].push_back(bitWeights);
}

// Add one image to the batch that will be sent to HW
void addImage(numeric::array & npData, unsigned int precision, 
	unsigned int padding, int simulation){

	int size = arraySize(npData);

	float * cData = (float *) ((PyArrayObject*) npData.ptr())->data;

	// Check that memory for batched data is not full
	assert(currentBatchSize < maxBatchSize);

	uint64_t startingWord = inputMemWords * currentBatchSize;

	std::cout << "Add image " << currentBatchSize << " - " << startingWord << std::endl;

	for(unsigned int e=0; e<arraySize(npData); e++){
		// Which bit will the current value be mapped to (starting bit)
		unsigned int bit = e * precision;
		
		// Where the starting bit is in the arra of words
		unsigned int word = bit / 64;
		// Which is the offset inside the particular word
		unsigned int offset = bit % 64;
		
		MemoryWord val = cData[e];

		assert(word < inputMemWords);
		assert(val>=0);

		inputData[startingWord + word] |= val << offset;
	}
	// Increment batch size
	currentBatchSize++;
}

void inference(unsigned int simulation){
	// Do Compute
	std::cout << "Do Compute" << std::endl;

	struct timeval t1,t2;
	unsigned long hwTime;

	#ifdef __SDX__
		SDx sdx;
		sdx.createBuffer(0, CL_MEM_READ_ONLY, sizeof(MemoryWord)*inputMemWords*currentBatchSize, 0);
		sdx.createBuffer(1, CL_MEM_WRITE_ONLY, sizeof(MemoryWord)*outputMemWords*currentBatchSize, 1);
		sdx.copyHostToDevice(0, inputData, sizeof(MemoryWord)*inputMemWords*currentBatchSize);
		sdx.buildKernel("BlackBoxJam_hw.xclbin", "opencldesign_wrapper");
		sdx.setArgs(sdx.getBufferAt(0), sdx.getBufferAt(1), false, currentBatchSize);
		auto duration = sdx.Launch();
		sdx.copyDeviceToHost(1, outputData, sizeof(MemoryWord)*outputMemWords*currentBatchSize);
		hwTime = duration/1000;	
	#else
		gettimeofday(&t1, NULL);
		opencldesign_wrapper((ap_uint<512> *)inputData, (ap_uint<512> *) outputData, false, currentBatchSize);
		gettimeofday(&t2, NULL);
		hwTime = (t2.tv_sec * 1000 * 1000 + t2.tv_usec) - (t1.tv_sec * 1000 * 1000 + t1.tv_usec);
	#endif


	std::cout << "HW Convolution only: " << (hwTime) << std::endl;
}

void fetchResult(numeric::array & classes, unsigned int precision, unsigned int simulation){
	float *pyClasses = getArrayData<float>(classes);
	
	std::cout << "Converting output" << std::endl;

	// Check that there are outputs to read
	assert(fetchedResults < currentBatchSize);

	// first word of the output to read
	uint64_t outWordOffset = outputMemWords * fetchedResults;

	std::cout << "Fetch result " << fetchedResults << " - " << outWordOffset << std::endl;

	for(unsigned int i=0; i < arraySize(classes)*precision / 64; i++){
		MemoryWord data;

		// read word
		data = outputData[(outWordOffset + i)];

		//std::cout <<  i << " " << std::hex << data << std::dec << std::endl;
		// Upack word in data of precision bits
		for(unsigned int j=0; j < 64/precision; j++){
			pyClasses[i*64/precision + j] = data & ((1<<precision) - 1);
			data = data >> precision;
		}
	}

	// Increment the counter of fetched results
	fetchedResults++;
}

// Instantiate buffers to support data movement of input/output of a given size and batch dimension.
// Sets up global parameters used in other functions.
// TODO: allocation of buffers for HW execution should be done here and not in FoldedMVInit()
void mallocBuffers(unsigned int inputValues, unsigned int inputPrecision, 
	unsigned int outputValues, unsigned int outputPrecision, unsigned int batchSize,
	unsigned int padding){

	// Number of padded words
	unsigned int inputWords = inputValues * inputPrecision / padding + ( ((inputValues * inputPrecision ) % padding) != 0 );
	unsigned int outputWords = outputValues * outputPrecision / padding + ( ((outputValues * outputPrecision ) % padding) != 0 );

	// Allocate buffers for input and output data
	inputData = (MemoryWord *) malloc( inputWords * padding / 64 * sizeof(MemoryWord) * batchSize);
	outputData = (MemoryWord *) malloc( outputWords * padding / 64 * sizeof(MemoryWord) * batchSize);

	assert(inputData!=NULL);
	assert(outputData!=NULL);

	// Set buffers to 0
	memset(inputData, 0, inputWords * padding / 64 * sizeof(MemoryWord)  * batchSize);
	memset(outputData, 0, outputWords * padding / 64 * sizeof(MemoryWord) * batchSize);

	// configuring global parameters
	inputMemWords = inputWords * padding/64;
	outputMemWords = outputWords * padding/64;

	printf("Input words = %llu\n", inputMemWords);
	printf("Output words = %llu\n", outputMemWords);

	maxBatchSize = batchSize;
	currentBatchSize = 0;
}


// Free allocated buffers
// TODO: if buffer for HW execution are allocated in the previous function they should be freed here.
void freeBuffers(){
	free(inputData);
	free(outputData);
}

// Init accelerator
void initAccelerator(unsigned int simulation){
	std::cout << "Starting accelerator" << std::endl;
	
	std::cout << "Accelerator configured" << std::endl;
}

// Interface to be exposed to python.
// All the functions listed here will be exposed to python and will bind to the corresponding function pointer.
BOOST_PYTHON_MODULE(rpnn)
{
	numeric::array::set_module_and_type("numpy", "ndarray");

    // Add regular functions to the module.
    def("inference", inference);
    def("initAccelerator", initAccelerator);
    def("dumpNetworkParameters", dumpNetworkParameters);
	def("createConvolutionalLayer", createConvolutionalLayer);
    def("mallocBuffers", mallocBuffers);
    def("freeBuffers", freeBuffers);
    def("addImage", addImage);
    def("fetchResult", fetchResult);
}
