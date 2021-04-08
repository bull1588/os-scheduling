#include <iostream>
#include <string>
#include <list>
#include <vector>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <unistd.h>
#include "configreader.h"
#include "process.h"

// Shared data for all cores
typedef struct SchedulerData {
    std::mutex mutex;
    std::condition_variable condition;
    ScheduleAlgorithm algorithm;
    uint32_t context_switch;
    uint32_t time_slice;
    std::list<Process*> ready_queue;
    bool all_terminated;
} SchedulerData;

void coreRunProcesses(uint8_t core_id, SchedulerData *data);
int printProcessOutput(std::vector<Process*>& processes, std::mutex& mutex);
void clearOutput(int num_lines);
uint64_t currentTime();
std::string processStateToString(Process::State state);

int main(int argc, char **argv)
{
    // Ensure user entered a command line parameter for configuration file name
    if (argc < 2)
    {
        std::cerr << "Error: must specify configuration file" << std::endl;
        exit(EXIT_FAILURE);
    }
    // Declare variables used throughout main
    int i;
    SchedulerData *shared_data;
    std::vector<Process*> processes;

    // Read configuration file for scheduling simulation
    SchedulerConfig *config = readConfigFile(argv[1]);

    // Store configuration parameters in shared data object
    uint8_t num_cores = config->cores;
    shared_data = new SchedulerData();
    shared_data->algorithm = config->algorithm;
    shared_data->context_switch = config->context_switch;
    shared_data->time_slice = config->time_slice;
    shared_data->all_terminated = false;

    // Create processes
    uint64_t start = currentTime();
    for (i = 0; i < config->num_processes; i++)
    {
        Process *p = new Process(config->processes[i], start);
        processes.push_back(p);
        // If process should be launched immediately, add to ready queue
        if (p->getState() == Process::State::Ready)
        {
            shared_data->ready_queue.push_back(p);
        }
    }
    // Free configuration data from memory
    deleteConfig(config);
    // Launch 1 scheduling thread per cpu core
    std::thread *schedule_threads = new std::thread[num_cores];
    for (i = 0; i < num_cores; i++)
    {
        schedule_threads[i] = std::thread(coreRunProcesses, i, shared_data);
    }

    // Main thread work goes here
    int num_lines = 0;
    int count_terminated = 0;
    while (!(shared_data->all_terminated)){
        // Clear output from previous iteration
        clearOutput(num_lines);
        for(int i = 0; i < processes.size(); i++){
            //1- get current time
            uint64_t current_time = currentTime(); 
            //2- *Check if any processes need to move from NotStarted to Ready (based on elapsed time), and if so put that process in the ready queue
            if((current_time - start >= processes[i]->getStartTime()) && (processes[i]->getState() == Process::State::NotStarted))
            { 
                processes[i]->setState(Process::State::Ready, current_time); //now in ready queue
                {
                std::lock_guard<std::mutex> lock(shared_data->mutex);
                shared_data->ready_queue.push_back(processes[i]);
                //std::cout << "Pushed to ready queue from NotStarted\n";
                }
            }
            //3- *Check if any processes have finished their I/O burst, and if so put that process back in the ready queue.
            //AKA if process is in I/O AND the time it has spent in current burst is GREATER than the time needed for the current burst
            if((processes[i]->getState() == Process::State::IO) 
                && (current_time - processes[i]->getBurstStartTime() >= processes[i]->getBurstTime(processes[i]->getCurrentBurst())))
            {
            processes[i]->setState(Process::State::Ready, currentTime()); //now in ready queue
            processes[i]->nextBurst(); //move to next since IO burst is over
                {
                    std::lock_guard<std::mutex> lock(shared_data->mutex);
                    shared_data->ready_queue.push_back(processes[i]);
                }
            }   
            //4- *Check if any running process need to be interrupted (RR time slice expires or newly ready process has higher priority)
            //if RR AND it has been on CPU for equal to or longer than the designated time slice
            if((processes[i]->getState() == Process::State::Running) 
                && (shared_data->algorithm == ScheduleAlgorithm::RR) 
                && (currentTime() - processes[i]->getBurstStartTime() >= shared_data->time_slice))
            { 
                processes[i]->interrupt(); //interrupt!
            }
            //if ready queue not empty AND current process is not lower priority than top of queue (indicated by a higher priority value)
            if((processes[i]->getState() == Process::State::Running) 
                && (!shared_data->ready_queue.empty()) 
                && (processes[i]->getPriority() > shared_data->ready_queue.front()->getPriority())
                && shared_data->algorithm == ScheduleAlgorithm::PP)
            {
                processes[i]->interrupt(); //interrupt!
            }
            //5- *Sort the ready queue (if needed - based on scheduling algorithm)
            if (shared_data->algorithm == ScheduleAlgorithm::SJF){
                {
                std::lock_guard<std::mutex> lock(shared_data->mutex);
                shared_data->ready_queue.sort(SjfComparator());
                }
            }
            if (shared_data->algorithm == ScheduleAlgorithm::PP){
                {
                std::lock_guard<std::mutex> lock(shared_data->mutex);
                shared_data->ready_queue.sort(PpComparator());
                }
            }
        }//for loop
        //6- Determine if all processes are in the terminated state
        count_terminated = 0;
        for(int i = 0; i < processes.size(); i++){ //check all to count number of terminated
            if (processes[i]->getState() == Process::State::Terminated){
                count_terminated++;
            }
        }
        if(count_terminated == processes.size()){
            {
            std::lock_guard<std::mutex> lock(shared_data->mutex);
            shared_data->all_terminated = true;
            }
        }

        // output process status table
        num_lines = printProcessOutput(processes, shared_data->mutex);
        // sleep 50 ms
        usleep(50000);
    }
    // wait for threads to finish
    for (i = 0; i < num_cores; i++)
    {
        schedule_threads[i].join();
    }

    // print final statistics
    double cpu_utilization = 0.0; //percentage
    double cpu_time = 0.0; //total
    double throughput_first_half = 0.0; 
    double throughput_second_half = 0.0; 
    double throughput_full = 0.0; 
    double avg_turnaround = 0.0; 
    double total_turnaround = 0.0;
    double avg_waiting_time = 0.0; 
    double total_waiting_time = 0.0;
    int count_processes = 0;
    std::vector<double> turnarounds;

    //loop through to get totals for all processes and count
    for(int i = 0; i < processes.size(); i++){
        cpu_time += processes[i]->getCpuTime();
        total_turnaround += processes[i]->getTurnaroundTime();
        total_waiting_time += processes[i]->getWaitTime();
        turnarounds.push_back(processes[i]->getTurnaroundTime()); //populate vector holding turn time for each process
        count_processes++;
    }
    avg_turnaround = total_turnaround / count_processes; //average it out
    avg_waiting_time = total_waiting_time / count_processes;
    cpu_utilization = (cpu_time / total_turnaround) * 100.0; //mult by 100 to get a percentage

    //now sort turnarounds to see which were the first half done and which were second half
    std::sort(turnarounds.begin(), turnarounds.end());
    int count_first_half = 0;
    int count_second_half = 0;
    for(int i = 0; i < processes.size(); i++){
        if(i < processes.size() / 2){ //first half
            throughput_first_half += turnarounds[i];
            count_first_half++;
        }
        else{ //second half
            throughput_second_half += turnarounds[i];
            count_second_half++;
        }
    }
    throughput_first_half = count_first_half / throughput_first_half;
    throughput_second_half = count_second_half / throughput_second_half;

    //print!
    printf("CPU utilization: %0.1lf percent \n", cpu_utilization);
    printf("Throughput average for first half: %0.3lf processes per second \n", throughput_first_half);
    printf("Throughput average for second half: %0.3lf processes per second \n", throughput_second_half);
    printf("Overall throughput average: %0.3lf processes per second \n", (throughput_first_half + throughput_second_half) / 2);
    printf("Average turnaround: %0.1lf seconds \n", avg_turnaround);
    printf("Average waiting time: %0.1lf seconds \n", avg_waiting_time);
    // Clean up before quitting program
    processes.clear();
    return 0;
}

void coreRunProcesses(uint8_t core_id, SchedulerData *shared_data)
{
    while(!shared_data->all_terminated){ //Run repeatedly until all processes are terminated. 
        Process *toRun;
        //Step 1 - Get process at front of ready queue.
        if(!shared_data->ready_queue.empty()){ //get a process if we have one waiting
            std::lock_guard<std::mutex> lock(shared_data->mutex); //if shared errors occur, consider moving this lock outside the IF to catch the read conditional
            toRun = shared_data->ready_queue.front(); 
            shared_data->ready_queue.pop_front(); //pop_front just evicts the front member, so we have to retrieve it first. 
        }
        if(toRun != NULL){
            toRun->setBurstStartTime(currentTime()); //Start "simulation" of CPU burst.
        }
        //Step 2 - Simulate the process until one of two conditionals occur - Burst time elapses or Interrupt is flagged
        while(toRun != NULL){ 
          toRun->setState(Process::State::Running, currentTime()); //set it to running at the current time flag.
          uint64_t time = currentTime();
          toRun->updateProcess(time);
          toRun->setCpuCore(core_id);
            if(toRun != NULL && time - toRun->getBurstStartTime() >= toRun->getBurstTime(toRun->getCurrentBurst())){
                toRun->nextBurst(); 
                toRun->setBurstStartTime(time);      
                if(toRun->getRemainingTime() > 0 && toRun->getCurrentBurst() < toRun->getNumBursts()){ 
                    toRun->setState(Process::State::IO, time); //if the process is unfinished when kicked, it goes to IO.
                    toRun->setCpuCore(-1);
                    toRun = NULL;
                } else {
                    toRun->setState(Process::State::Terminated, time); //if we finished the last burst, set process to Terminated.
                    toRun->setRemainingTime(0);
                    toRun->setCpuCore(-1); 
                    toRun = NULL;
                }
            } else if(toRun != NULL && toRun->isInterrupted()){
                std::lock_guard<std::mutex> lock(shared_data->mutex); 
                toRun->setState(Process::State::Ready, time);
                toRun->updateProcess(time);
                toRun->updateBurstTime(toRun->getCurrentBurst(), toRun->getBurstTime(toRun->getCurrentBurst()) - (time - toRun->getBurstStartTime())); //Update CPU Burst time (UNFINISHED)
                shared_data->ready_queue.push_back(toRun); //If an interrupt occured, send our process back into the ready queue
                toRun->interruptHandled(); //set it back to uninterrupted.
                toRun->setCpuCore(-1); 
                toRun = NULL;
            }
        }
        //Step 4 - Wait for the allotted context switching time.
        usleep(shared_data->context_switch);
    }
}

int printProcessOutput(std::vector<Process*>& processes, std::mutex& mutex)
{
    int i;
    int num_lines = 2;
    std::lock_guard<std::mutex> lock(mutex);
    printf("|   PID | Priority |      State | Core | Turn Time | Wait Time | CPU Time | Remain Time |\n");
    printf("+-------+----------+------------+------+-----------+-----------+----------+-------------+\n");
    for (i = 0; i < processes.size(); i++)
    {
        if (processes[i]->getState() != Process::State::NotStarted)
        {
            uint16_t pid = processes[i]->getPid();
            uint8_t priority = processes[i]->getPriority();
            std::string process_state = processStateToString(processes[i]->getState());
            int8_t core = processes[i]->getCpuCore();
            std::string cpu_core = (core >= 0) ? std::to_string(core) : "--";
            double turn_time = processes[i]->getTurnaroundTime();
            double wait_time = processes[i]->getWaitTime();
            double cpu_time = processes[i]->getCpuTime();
            double remain_time = processes[i]->getRemainingTime();
            printf("| %5u | %8u | %10s | %4s | %9.1lf | %9.1lf | %8.1lf | %11.1lf |\n", 
                   pid, priority, process_state.c_str(), cpu_core.c_str(), turn_time, 
                   wait_time, cpu_time, remain_time);
            num_lines++;
        }
    }
    return num_lines;
}

void clearOutput(int num_lines)
{
    int i;
    for (i = 0; i < num_lines; i++)
    {
        fputs("\033[A\033[2K", stdout);
    }
    rewind(stdout);
    fflush(stdout);
}

uint64_t currentTime()
{
    uint64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::system_clock::now().time_since_epoch()).count();
    return ms;
}

std::string processStateToString(Process::State state)
{
    std::string str;
    switch (state)
    {
        case Process::State::NotStarted:
            str = "not started";
            break;
        case Process::State::Ready:
            str = "ready";
            break;
        case Process::State::Running:
            str = "running";
            break;
        case Process::State::IO:
            str = "i/o";
            break;
        case Process::State::Terminated:
            str = "terminated";
            break;
        default:
            str = "unknown";
            break;
    }
    return str;
}
