/*
 * Copyright (C) 2014 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "DrawFrameTask.h"

#include <gui/TraceUtils.h>
#include <utils/Log.h>
#include <algorithm>

#include "../DeferredLayerUpdater.h"
#include "../DisplayList.h"
#include "../Properties.h"
#include "../RenderNode.h"
#include "CanvasContext.h"
#include "RenderThread.h"

namespace android {
namespace uirenderer {
namespace renderthread {

DrawFrameTask::DrawFrameTask()
        : mRenderThread(nullptr)
        , mContext(nullptr)
        , mContentDrawBounds(0, 0, 0, 0)
        , mSyncResult(SyncResult::OK) {}

DrawFrameTask::~DrawFrameTask() {}

void DrawFrameTask::setContext(RenderThread* thread, CanvasContext* context,
                               RenderNode* targetNode) {
    mRenderThread = thread;
    mContext = context;
    mTargetNode = targetNode;
}

void DrawFrameTask::setHintSessionCallbacks(std::function<void(int64_t)> updateTargetWorkDuration,
                                            std::function<void(int64_t)> reportActualWorkDuration) {
    mUpdateTargetWorkDuration = std::move(updateTargetWorkDuration);
    mReportActualWorkDuration = std::move(reportActualWorkDuration);
}

void DrawFrameTask::pushLayerUpdate(DeferredLayerUpdater* layer) {
    LOG_ALWAYS_FATAL_IF(!mContext,
                        "Lifecycle violation, there's no context to pushLayerUpdate with!");

    for (size_t i = 0; i < mLayers.size(); i++) {
        if (mLayers[i].get() == layer) {
            return;
        }
    }
    mLayers.push_back(layer);
}

void DrawFrameTask::removeLayerUpdate(DeferredLayerUpdater* layer) {
    for (size_t i = 0; i < mLayers.size(); i++) {
        if (mLayers[i].get() == layer) {
            mLayers.erase(mLayers.begin() + i);
            return;
        }
    }
}

int DrawFrameTask::drawFrame() {
    LOG_ALWAYS_FATAL_IF(!mContext, "Cannot drawFrame with no CanvasContext!");

    mSyncResult = SyncResult::OK;
    mSyncQueued = systemTime(SYSTEM_TIME_MONOTONIC);
    postAndWait();

    return mSyncResult;
}

void DrawFrameTask::postAndWait() {
    AutoMutex _lock(mLock);
    mRenderThread->queue().post([this]() { run(); });
    mSignal.wait(mLock);
}

void DrawFrameTask::run() {
    const int64_t vsyncId = mFrameInfo[static_cast<int>(FrameInfoIndex::FrameTimelineVsyncId)];
    ATRACE_FORMAT("DrawFrames %" PRId64, vsyncId);
    nsecs_t syncDelayDuration = systemTime(SYSTEM_TIME_MONOTONIC) - mSyncQueued;

    bool canUnblockUiThread;
    bool canDrawThisFrame;
    {
        TreeInfo info(TreeInfo::MODE_FULL, *mContext);
        canUnblockUiThread = syncFrameState(info);
        canDrawThisFrame = info.out.canDrawThisFrame;

        if (mFrameCompleteCallback) {
            mContext->addFrameCompleteListener(std::move(mFrameCompleteCallback));
            mFrameCompleteCallback = nullptr;
        }
    }

    // Grab a copy of everything we need
    CanvasContext* context = mContext;
    std::function<void(int64_t)> callback = std::move(mFrameCallback);
    mFrameCallback = nullptr;
    int64_t intendedVsync = mFrameInfo[static_cast<int>(FrameInfoIndex::IntendedVsync)];
    int64_t frameDeadline = mFrameInfo[static_cast<int>(FrameInfoIndex::FrameDeadline)];
    int64_t frameStartTime = mFrameInfo[static_cast<int>(FrameInfoIndex::FrameStartTime)];

    // From this point on anything in "this" is *UNSAFE TO ACCESS*
    if (canUnblockUiThread) {
        unblockUiThread();
    }

    // Even if we aren't drawing this vsync pulse the next frame number will still be accurate
    if (CC_UNLIKELY(callback)) {
        context->enqueueFrameWork(
                [callback, frameNr = context->getFrameNumber()]() { callback(frameNr); });
    }

    nsecs_t dequeueBufferDuration = 0;
    if (CC_LIKELY(canDrawThisFrame)) {
        dequeueBufferDuration = context->draw();
    } else {
        // wait on fences so tasks don't overlap next frame
        context->waitOnFences();
    }

    if (!canUnblockUiThread) {
        unblockUiThread();
    }

    // These member callbacks are effectively const as they are set once during init, so it's safe
    // to use these directly instead of making local copies.
    if (mUpdateTargetWorkDuration && mReportActualWorkDuration) {
        constexpr int64_t kSanityCheckLowerBound = 100000;       // 0.1ms
        constexpr int64_t kSanityCheckUpperBound = 10000000000;  // 10s
        int64_t targetWorkDuration = frameDeadline - intendedVsync;
        targetWorkDuration = targetWorkDuration * Properties::targetCpuTimePercentage / 100;
        if (targetWorkDuration > kSanityCheckLowerBound &&
            targetWorkDuration < kSanityCheckUpperBound &&
            targetWorkDuration != mLastTargetWorkDuration) {
            mLastTargetWorkDuration = targetWorkDuration;
            mUpdateTargetWorkDuration(targetWorkDuration);
        }
        int64_t frameDuration = systemTime(SYSTEM_TIME_MONOTONIC) - frameStartTime;
        int64_t actualDuration = frameDuration -
                                 (std::min(syncDelayDuration, mLastDequeueBufferDuration)) -
                                 dequeueBufferDuration;
        if (actualDuration > kSanityCheckLowerBound && actualDuration < kSanityCheckUpperBound) {
            mReportActualWorkDuration(actualDuration);
        }
    }
    mLastDequeueBufferDuration = dequeueBufferDuration;
}

bool DrawFrameTask::syncFrameState(TreeInfo& info) {
    ATRACE_CALL();
    int64_t vsync = mFrameInfo[static_cast<int>(FrameInfoIndex::Vsync)];
    int64_t intendedVsync = mFrameInfo[static_cast<int>(FrameInfoIndex::IntendedVsync)];
    int64_t vsyncId = mFrameInfo[static_cast<int>(FrameInfoIndex::FrameTimelineVsyncId)];
    int64_t frameDeadline = mFrameInfo[static_cast<int>(FrameInfoIndex::FrameDeadline)];
    int64_t frameInterval = mFrameInfo[static_cast<int>(FrameInfoIndex::FrameInterval)];
    mRenderThread->timeLord().vsyncReceived(vsync, intendedVsync, vsyncId, frameDeadline,
            frameInterval);
    bool canDraw = mContext->makeCurrent();
    mContext->unpinImages();

    for (size_t i = 0; i < mLayers.size(); i++) {
        mLayers[i]->apply();
    }
    mLayers.clear();
    mContext->setContentDrawBounds(mContentDrawBounds);
    mContext->prepareTree(info, mFrameInfo, mSyncQueued, mTargetNode);

    // This is after the prepareTree so that any pending operations
    // (RenderNode tree state, prefetched layers, etc...) will be flushed.
    if (CC_UNLIKELY(!mContext->hasSurface() || !canDraw)) {
        if (!mContext->hasSurface()) {
            mSyncResult |= SyncResult::LostSurfaceRewardIfFound;
        } else {
            // If we have a surface but can't draw we must be stopped
            mSyncResult |= SyncResult::ContextIsStopped;
        }
        info.out.canDrawThisFrame = false;
    }

    if (info.out.hasAnimations) {
        if (info.out.requiresUiRedraw) {
            mSyncResult |= SyncResult::UIRedrawRequired;
        }
    }
    if (!info.out.canDrawThisFrame) {
        mSyncResult |= SyncResult::FrameDropped;
    }
    // If prepareTextures is false, we ran out of texture cache space
    return info.prepareTextures;
}

void DrawFrameTask::unblockUiThread() {
    AutoMutex _lock(mLock);
    mSignal.signal();
}

} /* namespace renderthread */
} /* namespace uirenderer */
} /* namespace android */
