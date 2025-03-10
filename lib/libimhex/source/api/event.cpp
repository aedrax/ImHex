#include <hex/api/event.hpp>

namespace hex {

    std::map<void *, EventManager::EventList::iterator>& EventManager::getTokenStore() {
        static std::map<void *, EventManager::EventList::iterator> tokenStore;

        return tokenStore;
    }

    EventManager::EventList& EventManager::getEvents() {
        static EventManager::EventList events;

        return events;
    }


}