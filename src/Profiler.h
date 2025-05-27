/*
 * SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: LicenseRef-NvidiaProprietary
 *
 * NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
 * property and proprietary rights in and to this material, related
 * documentation and any modifications thereto. Any use, reproduction,
 * disclosure or distribution of this material and related documentation
 * without an express license agreement from NVIDIA CORPORATION or
 * its affiliates is strictly prohibited.
 */

#pragma once

#include <nvrhi/nvrhi.h>
#include <chrono>
#include <optional>
#include <queue>

// The AveragingTimerQuery class implements a timer query that is non-blocking using a pool of
// regular NVRHI TimerQueries, and that accumulates the timing results over a set time interval.
class AveragingTimerQuery
{
private:
    nvrhi::DeviceHandle m_device;
    std::queue<nvrhi::TimerQueryHandle> m_idleQueries;
    std::queue<nvrhi::TimerQueryHandle> m_activeQueries;
    nvrhi::TimerQueryHandle m_openQuery;

    std::vector<float> m_history;
    float m_updateIntervalSeconds = 0.5f;
    std::chrono::steady_clock::time_point m_lastUpdateTime = std::chrono::steady_clock::now();
    std::optional<float> m_averageTime;

public:
    AveragingTimerQuery(nvrhi::IDevice* device)
        : m_device(device)
    { }

    // Takes an available query from the pool and calls commandList->beginQuery with it.
    void beginQuery(nvrhi::ICommandList* commandList);

    // Calls commandList->endQuery with the currently open timer query.
    void endQuery(nvrhi::ICommandList* commandList);

    // Polls the active timer queries and retrieves available results, also processes temporal averaging.
    // Call update() on every frame.
    void update();

    // Sets the time interval between updating average time values.
    void setUpdateInterval(float seconds);

    // Clears the history, such as when changing rendering algorithms.
    void clearHistory();

    // Returns the latest directly measured time, if any.
    std::optional<float> getLatestAvailableTime();

    // Returns the latest average time, if any.
    std::optional<float> getAverageTime();
};