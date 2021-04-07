#include "process.h"

// Process class methods
Process::Process(ProcessDetails details, uint64_t current_time)
{
    int i;
    pid = details.pid;
    start_time = details.start_time;
    num_bursts = details.num_bursts;
    current_burst = 0;
    burst_times = new uint32_t[num_bursts];
    for (i = 0; i < num_bursts; i++)
    {
        burst_times[i] = details.burst_times[i];
    }
    priority = details.priority;
    state = (start_time == 0) ? State::Ready : State::NotStarted;
    if (state == State::Ready)
    {
        launch_time = current_time;
        ready_queue_start_time = current_time;//
    }
    is_interrupted = false;
    interrupt_time_completed = 0;//
    initial_remain_time = 0;//
    core = -1;
    turn_time = 0;
    wait_time = 0;
    cpu_time = 0;
    remain_time = 0;
    
    for (i = 0; i < num_bursts; i+=2)
    {
        remain_time += burst_times[i];
    }
    initial_remain_time = remain_time;//
}

Process::~Process()
{
    delete[] burst_times;
}

uint16_t Process::getPid() const
{
    return pid;
}

uint32_t Process::getStartTime() const
{
    return start_time;
}

uint8_t Process::getPriority() const
{
    return priority;
}

uint64_t Process::getBurstStartTime() const
{
    return burst_start_time;
}

Process::State Process::getState() const
{
    return state;
}

bool Process::isInterrupted() const
{
    return is_interrupted;
}

int8_t Process::getCpuCore() const
{
    return core;
}

double Process::getTurnaroundTime() const
{
    return (double)turn_time / 1000.0;
}

double Process::getWaitTime() const
{
    return (double)wait_time / 1000.0;
}

double Process::getCpuTime() const
{
    return (double)cpu_time / 1000.0;
}

double Process::getRemainingTime() const
{
    return (double)remain_time / 1000.0;
}

uint32_t Process::getBurstTime(int burst_idx) const
{
    return burst_times[burst_idx]; 
}

uint16_t Process::getCurrentBurst() const
{
    return current_burst;
}

uint16_t Process::getNumBursts() const
{
    return num_bursts;
}

void Process::setRemainingTime(int32_t time)
{
    remain_time = time;
}

void Process::setBurst(uint16_t burst_idx)
{
    current_burst = burst_idx;
}

void Process::nextBurst()
{
    current_burst++;
}

void Process::setBurstStartTime(uint64_t current_time)
{
    burst_start_time = current_time;
}

void Process::setState(State new_state, uint64_t current_time)
{
    if (state == State::NotStarted && new_state == State::Ready){
        launch_time = current_time;
    }
    if (new_state == State::Ready){
        ready_queue_start_time = current_time;
    }
    if (state == State::Ready && new_state != State::Ready){
        wait_time += (current_time - ready_queue_start_time);
    }
    state = new_state; //execute regardless
}

void Process::setCpuCore(int8_t core_num)
{
    core = core_num;
}

void Process::interrupt()
{
    is_interrupted = true;
}

void Process::interruptHandled()
{
    is_interrupted = false;
}

void Process::updateProcess(uint64_t current_time)
{
    // use `current_time` to update turnaround time, wait time, burst times, cpu time, and remaining time.
    //wait time and burst time already updated in setState() and updateBurstTime() respectively.

    //turn time
    if(state != State::Terminated){
       turn_time = 0;
       turn_time = current_time - launch_time;
    }

    //cpu time. first calulate cpu_time_completed. interrupt_time_completed done in updateBurstTime().
    uint32_t cpu_time_completed = 0;
    cpu_time = 0, remain_time = 0;
    if(current_burst != 0){
        for(int i = 0; i < current_burst; i+=2){ //+=2 because we only want to count the cpu time, not I/O
            cpu_time_completed += burst_times[i];
        }
    }
    if(current_burst % 2 == 0){
        cpu_time = (current_time - burst_start_time) + interrupt_time_completed + cpu_time_completed;
    }
    else{
        cpu_time = cpu_time_completed + interrupt_time_completed;
    }

    //remain time
    if (state == State::Terminated){ //if process is done
        remain_time = 0;
    }
    else{ //still going
        remain_time = initial_remain_time - cpu_time; //total time - time already on CPU
    }
}

void Process::updateBurstTime(int burst_idx, uint32_t new_time)
{
    interrupt_time_completed += burst_times[current_burst] - new_time;
    burst_times[burst_idx] = new_time; //
}

// Comparator methods: used in std::list sort() method
// No comparator needed for FCFS or RR (ready queue never sorted)

// SJF - comparator for sorting read queue based on shortest remaining CPU time
bool SjfComparator::operator ()(const Process *p1, const Process *p2)
{
    if(p1->getRemainingTime() < p2->getRemainingTime()){ //if p1 is shorter job
        return true;
    }
    return false;
}

// PP - comparator for sorting read queue based on priority
bool PpComparator::operator ()(const Process *p1, const Process *p2)
{
    if(p1->getPriority() < p2->getPriority()){ //if p1 is strictly HIGHER priority, since LOWER number indicates HIGHER priority
        return true;
    }
    return false; //p2 should maintain spot if they are equal.
}