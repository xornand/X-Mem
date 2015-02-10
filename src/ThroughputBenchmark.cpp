/* The MIT License (MIT)
 *
 * Copyright (c) 2014 Microsoft
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/**
 * @file
 * 
 * @brief Implementation file for the ThroughputBenchmark class.
 */

//Headers
#include <ThroughputBenchmark.h>
#include <common.h>
#include <ThroughputBenchmarkWorker.h>
#include <benchmark_kernels.h>
#include <Thread.h>
#include <Runnable.h>

//Libraries
#include <iostream>
#include <assert.h>
#include <time.h>

using namespace xmem;

ThroughputBenchmark::ThroughputBenchmark(
		void* mem_array,
		size_t len,
		uint32_t iterations,
#ifdef USE_SIZE_BASED_BENCHMARKS
		uint64_t passes_per_iteration,
#endif
		uint32_t num_worker_threads,
		uint32_t mem_node,
		uint32_t cpu_node,
		pattern_mode_t pattern_mode,
		rw_mode_t rw_mode,
		chunk_size_t chunk_size,
		int64_t stride_size,
		Timer& timer,
		std::vector<PowerReader*> dram_power_readers,
		std::string name
	) :
	Benchmark(
		mem_array,
		len,
		iterations,
#ifdef USE_SIZE_BASED_BENCHMARKS
		passes_per_iteration,
#endif
		num_worker_threads,
		mem_node,
		cpu_node,
		pattern_mode,
		rw_mode,
		chunk_size,
		stride_size,
		timer,
		dram_power_readers,
		"MB/s",
		name
	)
	{
}

bool ThroughputBenchmark::_run_core() {
	//Spit out useful info
	std::cout << std::endl;
	std::cout << "-------- Running Benchmark: " << _name;
	std::cout << " ----------" << std::endl;
	report_benchmark_info(); 
	
	//Set up kernel function pointers
	//TODO and FIXME: RandomFunctions!!!!
	SequentialFunction kernel_fptr;
	SequentialFunction kernel_dummy_fptr; 
	
	if (!determineSequentialKernel(_rw_mode, _chunk_size, _stride_size, &kernel_fptr, &kernel_dummy_fptr)) {
		std::cerr << "WARNING: Failed to find appropriate benchmark kernel." << std::endl;
		return false;
	}

	//Build indices for random workload
	//TODO and FIXME
//	if (_pattern_mode == RANDOM) 
//		_buildRandomPointerPermutation();

	//Start power measurement
	if (g_verbose) 
		std::cout << "Starting power measurement threads...";
	if (!_start_power_threads()) {
		if (g_verbose)
			std::cout << "FAIL" << std::endl;
		std::cerr << "WARNING: Failed to start power measurement threads." << std::endl;
	} else if (g_verbose)
		std::cout << "done" << std::endl;

	//Set up some stuff for worker threads
	size_t len_per_thread = _len / _num_worker_threads; //TODO: is this what we want?
	std::vector<ThroughputBenchmarkWorker*> workers;
	std::vector<Thread*> worker_threads;

	//Do a bunch of iterations of the core benchmark routines
	if (g_verbose)
		std::cout << "Running benchmark." << std::endl << std::endl;

	for (uint32_t i = 0; i < _iterations; i++) {
		//Create workers and worker threads
		workers.reserve(_num_worker_threads);
		worker_threads.reserve(_num_worker_threads);
		for (uint32_t t = 0; t < _num_worker_threads; t++) {
			void* thread_mem_array = reinterpret_cast<void*>(reinterpret_cast<uint8_t*>(_mem_array) + t * len_per_thread);
			int32_t cpu_id = cpu_id_in_numa_node(_cpu_node, t);
			if (cpu_id < 0)
				std::cerr << "WARNING: Failed to find logical CPU " << t << " in NUMA node " << _cpu_node << std::endl;
			workers.push_back(new ThroughputBenchmarkWorker(
												thread_mem_array,
												len_per_thread,
#ifdef USE_SIZE_BASED_BENCHMARKS
												_passes_per_iteration,
#endif
												kernel_fptr,
												kernel_dummy_fptr,
												cpu_id
											)
							);
			worker_threads.push_back(new Thread(workers[t]));
		}

		//Start worker threads! gogogo
		for (uint32_t t = 0; t < _num_worker_threads; t++)
			worker_threads[t]->create_and_start();

		//Wait for all threads to complete
		for (uint32_t t = 0; t < _num_worker_threads; t++)
			if (!worker_threads[t]->join())
				std::cerr << "WARNING: A worker thread failed to complete by the expected time!" << std::endl;

		//Compute throughput achieved with all workers
		uint64_t total_passes = 0;
		uint64_t total_adjusted_ticks = 0;
		uint64_t avg_adjusted_ticks = 0;
		uint64_t total_elapsed_dummy_ticks = 0;
		uint64_t bytes_per_pass = workers[0]->getBytesPerPass(); //all should be the same.
		bool iter_warning = false;
		for (uint32_t t = 0; t < _num_worker_threads; t++) {
			total_passes += workers[t]->getPasses();
			total_adjusted_ticks += workers[t]->getAdjustedTicks();
			total_elapsed_dummy_ticks += workers[t]->getElapsedDummyTicks();
			iter_warning |= workers[t]->hadWarning();
		}

		avg_adjusted_ticks = total_adjusted_ticks / _num_worker_threads;
			
		if (g_verbose ) { //Report duration for this iteration
			std::cout << "Iter " << i+1 << " had " << total_passes << " passes in total across " << _num_worker_threads << " threads, with " << bytes_per_pass << " bytes touched per pass:";
			if (iter_warning) std::cout << " -- WARNING";
			std::cout << std::endl;

			std::cout << "...clock ticks in total across " << _num_worker_threads << " threads == " << total_adjusted_ticks << " (adjusted by -" << total_elapsed_dummy_ticks << ")";
			if (iter_warning) std::cout << " -- WARNING";
			std::cout << std::endl;
			
			std::cout << "...ns in total across " << _num_worker_threads << " threads == " << total_adjusted_ticks * _timer.get_ns_per_tick() << " (adjusted by -" << total_elapsed_dummy_ticks * _timer.get_ns_per_tick() << ")";
			if (iter_warning) std::cout << " -- WARNING";
			std::cout << std::endl;

			std::cout << "...sec in total across " << _num_worker_threads << " threads == " << total_adjusted_ticks * _timer.get_ns_per_tick() / 1e9 << " (adjusted by -" << total_elapsed_dummy_ticks * _timer.get_ns_per_tick() / 1e9 << ")";
			if (iter_warning) std::cout << " -- WARNING";
			std::cout << std::endl;
		}
		
		//Compute metric for this iteration
		_metricOnIter[i] = ((static_cast<double>(total_passes) * static_cast<double>(bytes_per_pass)) / static_cast<double>(MB))   /   ((static_cast<double>(avg_adjusted_ticks) * _timer.get_ns_per_tick()) / 1e9);
		_averageMetric += _metricOnIter[i];


		//Clean up workers and threads for this iteration
		for (uint32_t t = 0; t < _num_worker_threads; t++) {
			delete worker_threads[t];
			delete workers[t];
		}
		worker_threads.clear();
		workers.clear();
	}

	//Stopping power measurement
	if (g_verbose) 
		std::cout << "Stopping power measurement threads...";
	if (!_stop_power_threads()) {
		if (g_verbose)
			std::cout << "FAIL" << std::endl;
		std::cerr << "WARNING: Failed to stop power measurement threads." << std::endl;
	} else if (g_verbose)
		std::cout << "done" << std::endl;
	
	//Run metadata
	_averageMetric /= static_cast<double>(_iterations);
	_hasRun = true;

	return true;
}
