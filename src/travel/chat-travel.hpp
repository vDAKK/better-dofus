#pragma once
#include "travel/chat-tp-links.hpp"
#include <cstdint>
#include <string>
void InstallChatTravel();
void StartChatTravel(ChatTpTarget target);
// Explicit map-menu selection: use this exact zaap, with no onward route.
void StartHavenbagZaap(int64_t map);
void TickChatTravel();
// Each frame: the phases where the trip only waits for a reply or a settled map.
void TickChatTravelFrame();
// The stability window of an arrival (chat_travel::SettleGapMs; the tests' sample by sample).
void SetChatTravelSettleGap(uint64_t milliseconds);
void RestoreChatTravel();
// A line in the native local chat history (no send, no network action), with
// its own prefix ("TP : ", "Metamob : "); same text within 2 s is shown once.
void PostChatNotice(const std::string& text);
