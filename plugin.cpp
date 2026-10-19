#include <Windows.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <chrono>
#include <mutex>
#include <thread>
#include <unordered_map>
#include "RE/B/BSCoreTypes.h"
#include "RE/B/BookMenu.h"
#include "RE/G/GFxMovieView.h"
#include "logger.h"
#include "GeneralFunctions.h"
#include "Serialization.h"
#include "mINIHelper.h"

//book base FormID -> saved left page number
std::unordered_map<RE::FormID, int> savedBookMarks;
std::mutex savedBookMarksMutex;

//state for the currently open book. Only touched on the main / UI thread.
RE::FormID openedBookId = 0;
int currentiLeftPageNumber = 0;

//bookmarks map access================================================================================================================================================
int GetSavedBookMark(RE::FormID bookId) {
	savedBookMarksMutex.lock();
	auto it = savedBookMarks.find(bookId);
	int page = (it != savedBookMarks.end()) ? it->second : 0;
	savedBookMarksMutex.unlock();
	return page;
}

void SetSavedBookMark(RE::FormID bookId, int page) {
	savedBookMarksMutex.lock();
	if (page > 0) {
		savedBookMarks[bookId] = page;
	}
	else {
		savedBookMarks.erase(bookId);
	}
	savedBookMarksMutex.unlock();
}

//book menu movie=====================================================================================================================================================
//returns _root.BookMenu_mc from the open Book Menu's book.swf, or an undefined GFxValue if it isn't available
RE::GFxValue GetBookMc() {
	RE::GFxValue bookMc;
	auto* ui = RE::UI::GetSingleton();
	RE::GPtr<RE::BookMenu> bookMenu = ui ? ui->GetMenu<RE::BookMenu>(RE::BookMenu::MENU_NAME) : nullptr;
	RE::GFxMovieView* movie = bookMenu ? bookMenu->GetRuntimeData().book.get() : nullptr;
	if (movie) {
		movie->GetVariable(&bookMc, "_root.BookMenu_mc");
	}
	return bookMc;
}

void SaveiLeftPageNumber() {
	RE::GFxValue bookMc = GetBookMc();
	if (!bookMc.IsDisplayObject()) {
		return;
	}

	RE::GFxValue iLeftPageNumber;
	if (bookMc.GetMember("iLeftPageNumber", &iLeftPageNumber) && iLeftPageNumber.IsNumber()) {
		currentiLeftPageNumber = static_cast<int>(iLeftPageNumber.GetNumber());
	}
	
	// auto* ui = RE::UI::GetSingleton();
	// RE::GPtr<RE::BookMenu> bookMenu = ui ? ui->GetMenu<RE::BookMenu>(RE::BookMenu::MENU_NAME) : nullptr;

	// RE::GFxValue iPageSetIndex;
	// if (bookMc.GetMember("iPageSetIndex", &iPageSetIndex) && iPageSetIndex.IsNumber()) {
	// 	currentiLeftPageNumber = static_cast<int>(iPageSetIndex.GetNumber());
	// }

	// if (bookMenu) {
	// 	auto& rd = bookMenu->GetRuntimeData();
	// 	logger::debug("left[{}] setIndex[{}] | pad7C[{}] pad97[{}] unk90[{}] closeMenu[{}] isNote[{}]",
	// 		currentiLeftPageNumber,
	// 		iPageSetIndex.IsNumber() ? iPageSetIndex.GetNumber() : -1.0,
	// 		rd.pad7C, rd.pad97, rd.unk90, rd.closeMenu, rd.isNote);
	// }
}

//Returns true when finished (restored, or nothing left to do). Returns false if pagination isn't done yet.
//Returns true when finished (restored, or nothing left to do). Returns false if pagination isn't done yet.
bool TryRestorePage(RE::FormID bookId, int page) {
	if (openedBookId != bookId) {
		return true; //menu closed or a different book was opened
	}

	RE::GFxValue bookMc = GetBookMc();
	if (!bookMc.IsDisplayObject()) {
		return false;
	}

	//wait for SetBookText + CalculatePagination to finish
	RE::GFxValue paginationIndex, pageInfo;
	if (!bookMc.GetMember("iPaginationIndex", &paginationIndex) || !paginationIndex.IsNumber() ||
		paginationIndex.GetNumber() != -1 ||
		!bookMc.GetMember("PageInfoA", &pageInfo) || !pageInfo.IsArray() || pageInfo.GetArraySize() <= 1) {
		return false;
	}

	//SetLeftPageNumber only accepts pages that exist
	RE::GFxValue arg;
	arg.SetNumber(static_cast<double>(page));
	bookMc.Invoke("SetLeftPageNumber", nullptr, &arg, 1);

	RE::GFxValue left;
	if (!bookMc.GetMember("iLeftPageNumber", &left) || !left.IsNumber() ||
		static_cast<int>(left.GetNumber()) != page) {
		//text got shorter since the bookmark was saved
		currentiLeftPageNumber = 0;
		logger::warn("page [{}] no longer valid for [{:08X}]", page, bookId);
		return true;
	}

	//make the bookmarked page the start of the cached set, like PrepForClose does,
	//so the engine's offsets 0/1 show it
	bookMc.SetMember("iPageSetIndex", arg);
	bookMc.Invoke("UpdatePages", nullptr, nullptr, 0);

	currentiLeftPageNumber = page;
	logger::debug("restored page [{}] for [{:08X}]", page, bookId);
	return true;
}

//Retries on a short timer. Re-queuing a UI task directly from inside a UI task could run
//every retry in the same frame, because SKSE drains the UI queue until it's empty.
void QueueRestorePage(RE::FormID bookId, int page, int attempt = 0) {
	constexpr int kMaxAttempts = 80; //~2 seconds

	std::thread([=]() {
		std::this_thread::sleep_for(std::chrono::milliseconds(25)); 

		auto* tasks = SKSE::GetTaskInterface();
		if (!tasks) {
			return;
		}

		tasks->AddUITask([=]() {
			if (!TryRestorePage(bookId, page)) {
				if (attempt < kMaxAttempts) {
					QueueRestorePage(bookId, page, attempt + 1);
				}
				else {
					logger::warn("gave up restoring page [{}] for [{:08X}]", page, bookId);
				}
			}
		});
	}).detach();
}

//event sinks=========================================================================================================================================================
class InputEventSink : public RE::BSTEventSink<RE::InputEvent*> {
public:
	static InputEventSink* GetSingleton() {
		static InputEventSink singleton;
		return &singleton;
	}

	bool sinkAdded = false;

	RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* eventPtr, RE::BSTEventSource<RE::InputEvent*>*) override {
		if (!eventPtr) {
			return RE::BSEventNotifyControl::kContinue;
		}

		//events come as a linked list -- one frame can hold several
		for (auto* event = *eventPtr; event; event = event->next) {
			if (event->eventType != RE::INPUT_EVENT_TYPE::kButton) {
				continue;
			}

			auto* buttonEvent = event->AsButtonEvent();
			if (buttonEvent && buttonEvent->IsUp()) {
				SaveiLeftPageNumber();
				break;
			}
		}

		return RE::BSEventNotifyControl::kContinue;
	}
};

class MenuEventSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent> {
public:
	static MenuEventSink* GetSingleton() {
		static MenuEventSink singleton;
		return &singleton;
	}

	bool sinkAdded = false;

	RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override {
		if (!event || event->menuName != RE::BookMenu::MENU_NAME) {
			return RE::BSEventNotifyControl::kContinue;
		}

		auto* inputManager = RE::BSInputDeviceManager::GetSingleton();
		auto* inputSink = InputEventSink::GetSingleton();

		//closing -- doesn't need the movie, the page was captured on button up
		if (!event->opening) {
			if (openedBookId != 0) {
				SetSavedBookMark(openedBookId, currentiLeftPageNumber);
				logger::debug("bookmark [{}] saved for [{:08X}]", currentiLeftPageNumber, openedBookId);
			}
			openedBookId = 0;
			currentiLeftPageNumber = 0;

			if (inputManager && inputSink->sinkAdded) {
				inputManager->RemoveEventSink(inputSink);
				inputSink->sinkAdded = false;
			}
		}
		else { //opening
			auto* book = RE::BookMenu::GetTargetForm();
			if (!gfuncs::IsFormValid(book)) {
				logger::warn("BookMenuOpenEvent: book not valid");
				return RE::BSEventNotifyControl::kContinue;
			}

			openedBookId = book->GetFormID();
			currentiLeftPageNumber = 0;

			logger::trace("Book Menu opened for [{}]", gfuncs::GetFormNameAndId(book));

			if (inputManager && !inputSink->sinkAdded) {
				inputManager->AddEventSink(inputSink);
				inputSink->sinkAdded = true;
			}

			int savedPage = GetSavedBookMark(openedBookId);
			if (savedPage > 0) {
				//keep the bookmark if the book is closed before the restore lands
				currentiLeftPageNumber = savedPage;
				QueueRestorePage(openedBookId, savedPage);
			}
		}
		
		return RE::BSEventNotifyControl::kContinue;
	}
};

void InstallMenuEventSink() {
	auto* sink = MenuEventSink::GetSingleton();
	if (sink->sinkAdded) {
		return;
	}

	if (auto* ui = RE::UI::GetSingleton()) {
		ui->AddEventSink<RE::MenuOpenCloseEvent>(sink);
		sink->sinkAdded = true;
	}
}

void MessageListener(SKSE::MessagingInterface::Message* message) {
	switch (message->type) {
		// Descriptions are taken from the original skse64 library
		// See:
		// https://github.com/ianpatt/skse64/blob/09f520a2433747f33ae7d7c15b1164ca198932c3/skse64/PluginAPI.h#L193-L212

		case SKSE::MessagingInterface::kDataLoaded: {
			InstallMenuEventSink();
			RE::ConsoleLog::GetSingleton()->Print("SSEBookmarks Installed");
			logger::info("SSEBookmarks Installed");
			break;
		}
	}
}

//serialization=======================================================================================================================================================
constexpr std::uint32_t kBookMarksRecord = 'BKMK';
constexpr std::uint32_t kBookMarksVersion = 1;

void LoadCallback(SKSE::SerializationInterface* ssi) {
	logger::trace("LoadCallback started");

	if (!ssi) {
		logger::error("ssi doesn't exist, aborting load.");
		return;
	}

	savedBookMarksMutex.lock();
	savedBookMarks.clear();

	std::uint32_t record, version, length;
	while (ssi->GetNextRecordInfo(record, version, length)) {
		logger::trace("record[{}]", gfuncs::uint32_to_string(record));

		if (record != kBookMarksRecord) {
			continue;
		}

		if (version != kBookMarksVersion) {
			logger::warn("bookmarks record version [{}] not recognized, skipping", version);
			continue;
		}

		std::uint32_t count = 0;
		if (!ssi->ReadRecordData(count)) {
			logger::error("failed to read bookmark count");
			continue;
		}

		for (std::uint32_t i = 0; i < count; i++) {
			RE::FormID oldId = 0;
			RE::FormID newId = 0;
			std::int32_t page = 0;

			if (!ssi->ReadRecordData(oldId) || !ssi->ReadRecordData(page)) {
				logger::error("failed to read bookmark [{}]", i);
				break;
			}

			//skip books whose plugin was removed
			if (!ssi->ResolveFormID(oldId, newId)) {
				logger::debug("couldn't resolve book [{:08X}], bookmark dropped", oldId);
				continue;
			}

			if (page > 0) {
				savedBookMarks[newId] = page;
			}
		}

		logger::debug("loaded [{}] bookmarks", savedBookMarks.size());
	}

	savedBookMarksMutex.unlock();
	logger::trace("LoadCallback complete");
}

void SaveCallback(SKSE::SerializationInterface* ssi) {
	logger::trace("SaveCallback started");

	if (!ssi) {
		logger::error("ssi doesn't exist, aborting save.");
		return;
	}

	if (!ssi->OpenRecord(kBookMarksRecord, kBookMarksVersion)) {
		logger::error("failed to open bookmarks record");
		return;
	}

	savedBookMarksMutex.lock();

	std::uint32_t count = static_cast<std::uint32_t>(savedBookMarks.size());
	ssi->WriteRecordData(count);

	for (auto& [bookId, page] : savedBookMarks) {
		std::int32_t page32 = page;
		ssi->WriteRecordData(bookId);
		ssi->WriteRecordData(page32);
	}

	savedBookMarksMutex.unlock();

	logger::debug("SaveCallback complete, saved [{}] bookmarks", count);
}

void RevertCallback(SKSE::SerializationInterface*) {
	savedBookMarksMutex.lock();
	savedBookMarks.clear();
	savedBookMarksMutex.unlock();
}

//init================================================================================================================================================================
SKSEPluginLoad(const SKSE::LoadInterface* skse) {
	SKSE::Init(skse);
	REL::Module::reset(); //make sure REL offsets are correct before touching them. Caused ctd when opening the game without this.

	SetupLog("Data/SKSE/Plugins/SSEBookmarks.ini");

	SKSE::GetMessagingInterface()->RegisterListener(MessageListener);

	auto* serialization = SKSE::GetSerializationInterface();
	serialization->SetUniqueID('EBm7');
	serialization->SetSaveCallback(SaveCallback);
	serialization->SetLoadCallback(LoadCallback);
	serialization->SetRevertCallback(RevertCallback);

	return true;
}