#include "ControllerAI.h"

#include "ControllerAISerialization.h"

#include "ExternalAI/Interface/AISEvents.h"

#include "Game.h"

namespace controllerai {

int CControllerAI::HandleEvent(int topic, const void* data) {
    if (!callback || skirmishAIId == -1) {
        return 0;
    }

    if (released && topic != EVENT_RELEASE) {
        return 0;
    }

    if (topic != EVENT_RELEASE) {
        EnsureInterfacesInitialized();
        ProcessSettings();
        ProcessQueries();
    }

    if (topic == EVENT_RELEASE) {
        const int reason = data ? static_cast<const SReleaseEvent*>(data)->reason : 0;
        Release(reason);
        return 0;
    }

    if (topic == EVENT_INIT) {
        CacheStaticData();
        UpdateObservation();
        WaitForResume();
        return 0;
    }

    json event = detail::EventToJson(topic, data);
    if (!event.is_null() && event.contains("topic")) {
        eventBuffer.push_back(event);
    }

    if (topic == EVENT_UPDATE) {
        const int frame = data ? static_cast<const SUpdateEvent*>(data)->frame : -1;
        const bool shouldPublish = (frame < 0) || ShouldPublishObservationForFrame(frame);
        if (frame >= 0) {
            frameFinished = !shouldPublish;
        }

        ProcessCommands();
        if (shouldPublish) {
            UpdateObservation();
            WaitForResume();
        }
    }

    return 0;
}

} // namespace controllerai