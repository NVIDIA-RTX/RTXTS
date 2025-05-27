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

#include "Profiler.h"


void AveragingTimerQuery::beginQuery(nvrhi::ICommandList* commandList)
{
    nvrhi::TimerQueryHandle query;
    if (m_idleQueries.empty())
    {
        query = m_device->createTimerQuery();
    }
    else
    {
        query = m_idleQueries.front();
        m_idleQueries.pop();
    }

    commandList->beginTimerQuery(query);
    m_openQuery = query;
}

void AveragingTimerQuery::endQuery(nvrhi::ICommandList* commandList)
{
    assert(m_openQuery);
    commandList->endTimerQuery(m_openQuery);
    m_activeQueries.push(m_openQuery);
    m_openQuery = nullptr;
}

void AveragingTimerQuery::update()
{
    while (!m_activeQueries.empty())
    {
        nvrhi::TimerQueryHandle query = m_activeQueries.front();
        if (m_device->pollTimerQuery(query))
        {
            float time = m_device->getTimerQueryTime(query);
            m_history.push_back(time);
            m_activeQueries.pop();
            m_idleQueries.push(query);
        }
        else
            break;
    }

    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    float const secondsSinceUpdate = std::chrono::duration_cast<std::chrono::microseconds>(now - m_lastUpdateTime)
        .count() * 1e-6f;

    if (secondsSinceUpdate > m_updateIntervalSeconds && !m_history.empty())
    {
        float sumTime = 0.f;
        for (float time : m_history)
            sumTime += time;
        
        m_averageTime = sumTime / m_history.size();
        m_lastUpdateTime = now;

        float const latestTime = m_history[m_history.size() - 1];
        m_history.clear();
        m_history.push_back(latestTime);
    }
}

void AveragingTimerQuery::setUpdateInterval(float seconds)
{
    m_updateIntervalSeconds = seconds;
}

void AveragingTimerQuery::clearHistory()
{
    m_history.clear();
    m_lastUpdateTime = std::chrono::steady_clock::now();
}

std::optional<float> AveragingTimerQuery::getLatestAvailableTime()
{
    return m_history.empty() ? std::optional<float>() :  m_history.back();
}

std::optional<float> AveragingTimerQuery::getAverageTime()
{
    return m_averageTime;
}