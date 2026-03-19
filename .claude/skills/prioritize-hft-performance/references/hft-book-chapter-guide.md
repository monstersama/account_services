# HFT Book Chapter Guide

Use this file as a navigation map for the bundled PDF instead of opening the full book blindly.

## Source

- Book: `Developing High-Frequency Trading Systems`
- PDF: `Developing-High-Frequency-Trading-Systems-Learn-how-to-implement-high-frequency-trading-from-scratch-with-C++or-Java-basics.pdf`
- Goal: identify the right chapter first, then open only the relevant section of the PDF

## Part Map

- Part 1: market structure, trading strategies, trading-system roles, exchange behavior
- Part 2: hardware, OS, networking, latency sources, lock-free design, logging, performance measurement
- Part 3: implementation choices in C++, Java, Python, FPGA, crypto, and cloud deployment

## Quick Lookup

- If you need market-structure context, trading-style vocabulary, or why HFT is different from regular trading: read Chapter 1.
- If you need trading-system architecture, market-data flow, order handling, OMS responsibilities, or component boundaries: read Chapter 2.
- If you need exchange and matching-engine behavior, order-book rules, or fill scenarios: read Chapter 3.
- If you need CPU, memory, OS, system-call, or scheduling background for low-latency design: read Chapter 4.
- If you need wire protocols, network stack behavior, packet life cycle, time sync, or monitoring: read Chapter 5.
- If you need context-switch reduction, lock-free thinking, prefetching, or memory pre-allocation: read Chapter 6.
- If you need kernel bypass, logging strategy, online statistics, or latency measurement: read Chapter 7.
- If you need C++-specific low-latency guidance around memory model, templates, constexpr, exceptions, or static analysis: read Chapter 8.
- If you need Java/JVM low-latency guidance around GC, warmup, queues, or thread models: read Chapter 9.
- If you need Python interoperability or how Python can stay useful in an HFT stack: read Chapter 10.
- If you need FPGA, crypto-market differences, or cloud deployment considerations: read Chapter 11.

## Chapter Summaries

### Chapter 1: Fundamentals of a High-Frequency Trading System

- Focus: HFT history, market participants, latency requirements, and common strategy families.
- Read this when: you need to classify a strategy, explain co-location and latency pressure, or understand terms such as market making, latency arbitrage, rebates, and pinging.
- Less useful when: you already know the domain and need implementation detail.
- Approximate book range: pp. 1-20.

### Chapter 2: The Critical Components of a Trading System

- Focus: the moving parts of a trading system, including gateways, APIs, order book handling, strategy logic, OMS, command and control, and services.
- Read this when: you are deciding where a responsibility should live in the system, how market data and orders flow through components, or what belongs on the critical path.
- Less useful when: the problem is primarily hardware, kernel, or language specific.
- Approximate book range: pp. 22-39.

### Chapter 3: Understanding the Trading Exchange Dynamics

- Focus: exchange architecture, order books, matching-engine behavior, and fill scenarios.
- Read this when: you are implementing exchange adapters, order-state transitions, matching semantics, or queue-position logic.
- Less useful when: you are optimizing local process latency instead of exchange behavior.
- Approximate book range: pp. 42-53.

### Chapter 4: HFT System Foundations - From Hardware to OS

- Focus: processors, RAM, shared memory, I/O, paging, system calls, threads, interrupts, compiler roles, linking, and process scheduling.
- Read this when: you need a foundation for why hardware and OS choices shape latency, jitter, and throughput.
- Less useful when: you only need a language-level idiom and already understand the machine model.
- Approximate book range: pp. 58-75.

### Chapter 5: Networking in Motion

- Focus: network models, packet life cycle, switches, Ethernet, IPv4, UDP/TCP, financial protocols, monitoring, packet capture, and time distribution.
- Read this when: the issue involves gateways, packet parsing, wire latency, feed handling, FIX, or clock synchronization.
- Less useful when: the bottleneck is purely in in-process memory layout or CPU execution.
- Approximate book range: pp. 78-110.

### Chapter 6: HFT Optimization - Architecture and Operating System

- Focus: performance mental models, context switches, lock-free data structures, synchronization costs, memory hierarchy, prefetching, and pre-allocation.
- Read this when: you are deciding whether to lock, how to eliminate scheduler interference, or how to avoid dynamic allocation on the hot path.
- Less useful when: you need exchange microstructure rather than systems optimization.
- Approximate book range: pp. 112-135.

### Chapter 7: HFT Optimization - Logging, Performance, and Networking

- Focus: kernel space versus user space, kernel bypass, logging and online statistics, latency-measurement methods, memory-mapped files, and low-latency transport media.
- Read this when: you need observability without destroying latency, want to measure performance correctly, or need to understand bypass techniques and link media tradeoffs.
- Less useful when: the task is about trading logic rather than platform behavior.
- Approximate book range: pp. 138-161.

### Chapter 8: C++ - The Quest for Microsecond Latency

- Focus: C++ memory model, removing runtime decisions, constexpr, exception cost, templates, STL tradeoffs, and static analysis for low-latency systems.
- Read this when: you are choosing between virtual dispatch and templates, evaluating exception use, reasoning about allocation and cache behavior, or reviewing C++ hot-path code.
- Less useful when: the implementation is in Java, Python, or hardware.
- Approximate book range: pp. 166-201.

### Chapter 9: Java and JVM for Low-Latency Systems

- Focus: Java basics for low latency, GC reduction, JVM warmup, compilation tiers, startup tuning, measurement, threading, queues, disruptor-style patterns, logging, and DB access.
- Read this when: you need to make a Java trading component more predictable under latency pressure.
- Less useful when: the stack is native C++ and the JVM is not involved.
- Approximate book range: pp. 204-232.

### Chapter 10: Python - Interpreted but Open to High Performance

- Focus: where Python fits in HFT, why it is slow, and how to pair it with C++ through bindings and FFI.
- Read this when: you need analytics, research, glue code, or controlled Python integration without putting pure Python on the hottest path.
- Less useful when: the problem requires a fully native low-latency execution path.
- Approximate book range: pp. 234-248.

### Chapter 11: High-Frequency FPGA and Crypto

- Focus: FPGA latency reduction, crypto-market mechanics, how crypto differs from traditional markets, crypto HFT strategies, and cloud-based trading-system deployment.
- Read this when: you are evaluating hardware acceleration, crypto exchange behavior, or deployment choices outside traditional colocated equities or futures setups.
- Less useful when: your task is limited to conventional exchange adapters or in-process optimization.
- Approximate book range: pp. 250-280.

## Suggested Reading Order For This Repository

- Start with Chapter 2 for component boundaries in `src/core/`, `src/order/`, `src/portfolio/`, `src/risk/`, and `gateway/src/`.
- Jump to Chapter 3 when touching order-state transitions, matching assumptions, or exchange-facing behavior.
- Use Chapters 4 to 7 for hot-path work in shared memory, event loops, networking, scheduling, and lock-free coordination.
- Use Chapter 8 for C++ implementation decisions in this repository.
- Use Chapters 9 to 11 only when the task explicitly crosses into JVM, Python, FPGA, crypto, or cloud topics.

## How To Use This Guide With The Skill

1. Read this file first to choose the smallest relevant chapter set.
2. Open the PDF only after you know which chapter matches the task.
3. Prefer the repository-specific rule files first:
   `hft-design-checklist.md` for direct review prompts.
4. Use the book when you need background, rationale, or a deeper explanation than the local checklist provides.
