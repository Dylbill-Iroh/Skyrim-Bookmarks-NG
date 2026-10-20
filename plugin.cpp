#include <Windows.h>
#include <algorithm>
#include <array>
#include <format>
#include <spdlog/sinks/basic_file_sink.h>
#include <chrono>
#include <mutex>
#include <string.h>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>
#include "RE/B/BSCoreTypes.h"
#include "RE/B/BarterMenu.h"
#include "RE/B/BookMenu.h"
#include "RE/C/ContainerMenu.h"
#include "RE/C/CraftingMenu.h"
#include "RE/F/FavoritesMenu.h"
#include "RE/G/GFxMovieView.h"
#include "RE/G/GiftMenu.h"
#include "RE/I/InventoryMenu.h"
#include "RE/T/TESForm.h"
#include "logger.h"
#include "GeneralFunctions.h"
#include "Serialization.h"
#include "mINIHelper.h"

struct BookData {
	int currentPage = 0;     //iLeftPageNumber to restore (0 based)
	int maxPageReached = 0;  //furthest page seen, 1 based, across all reading sessions
	int numOfPages = 0;      //total pages, 0 = not known yet
	bool nameChanged = false;
};

//book base FormID -> saved data. A book has an entry once this plugin has seen it open.
std::unordered_map<RE::FormID, BookData> savedBookMarks;
std::mutex savedBookMarksMutex;

bool bShowProgress = false;
bool bShowIcon = false;

//state for the currently open book. Only touched on the main / UI thread.
RE::FormID openedBookId = 0;
int currentiLeftPageNumber = 0;
int currentMaxPages = -1;     //-1 until pagination finishes
int currentPagesReached = 0;  //furthest page reached during this reading session, 1 based

void GetSettingsFromIni() {
	mINI::INIFile file("Data/SKSE/Plugins/SSEBookmarks.ini");
	mINI::INIStructure ini;
	file.read(ini);

	bShowProgress = mINI::GetIniBool(ini, "Main", "bShowProgress");
	bShowIcon = mINI::GetIniBool(ini, "Main", "bShowIcon");
}

[[nodiscard]] constexpr bool IsItemMenu(std::string_view MENU_NAME) noexcept {
	constexpr std::array targets{
		RE::InventoryMenu::MENU_NAME,  //InventoryMenu
		RE::ContainerMenu::MENU_NAME,  //ContainerMenu
		RE::BarterMenu::MENU_NAME,     //BarterMenu
		RE::CraftingMenu::MENU_NAME,   //Crafting Menu
		RE::FavoritesMenu::MENU_NAME,  //FavoritesMenu
		RE::GiftMenu::MENU_NAME        //GiftMenu
	};

	return std::find(targets.begin(), targets.end(), MENU_NAME) != targets.end();
}

//progress tags ========================================================================================================================================================
//example tag: " | (5 / 19)"

std::string RemoveProgressTag(std::string name) {
	auto npos = std::string::npos;
	auto iStart = name.find(" | (");
	if (iStart == npos) {
		return name;
	}

	auto iEnd = name.find(")", iStart);
	if (iEnd == npos) {
		return name;
	}

	std::string nameStart = name.substr(0, iStart);
	std::string nameEnd = name.substr(iEnd + 1);

	return nameStart + nameEnd;
}

std::string AddProgressTag(std::string name, const BookData& data) {
	name = RemoveProgressTag(name);
	return std::format("{} | ({} / {})", name, data.maxPageReached, data.numOfPages);
}

//Adds, updates or removes the " | (5 / 19)" tag on the book's name.
//Must be called WITHOUT savedBookMarksMutex held: renaming refreshes the item menu, which runs
//the formatName wrapper on this thread, and that takes the mutex.
void UpdateProgressTag(RE::FormID formId, bool refreshMenu) {
	savedBookMarksMutex.lock();
	auto it = savedBookMarks.find(formId);
	if (it == savedBookMarks.end()) {
		savedBookMarksMutex.unlock();
		return;
	}
	BookData data = it->second;
	savedBookMarksMutex.unlock();

	RE::TESForm* form = RE::TESForm::LookupByID(formId);
	if (!gfuncs::IsFormValid(form)) {
		return;
	}

	std::string name = form->GetName();
	std::string newName = name;
	bool nameChanged = data.nameChanged;

	if (bShowProgress && data.numOfPages > 0) {
		newName = AddProgressTag(name, data);
		nameChanged = true;
	}
	else if (data.nameChanged) {
		newName = RemoveProgressTag(name);
		nameChanged = false;
	}

	if (nameChanged != data.nameChanged) {
		savedBookMarksMutex.lock();
		data.nameChanged = nameChanged;
		savedBookMarks[formId] = data;
		savedBookMarksMutex.unlock();
	}

	if (newName != name) {
		gfuncs::SetFormName(form, newName);
		if (refreshMenu) {
			gfuncs::RefreshItemMenu();
		}
	}
}

//bookmarks map access================================================================================================================================================
int GetSavedBookMark(RE::FormID bookId) {
	savedBookMarksMutex.lock();
	auto it = savedBookMarks.find(bookId);
	int page = (it != savedBookMarks.end()) ? it->second.currentPage : 0;
	savedBookMarksMutex.unlock();
	return page;
}

//Stores the bookmark and merges this session's progress into the saved progress.
//maxPageReached only ever goes up.
void SetSavedBookMark(RE::FormID bookId, int page, int pagesReached, int numPages) {
	bool tagChanged = false;

	savedBookMarksMutex.lock();

	BookData data = savedBookMarks[bookId];   //creates the entry: the book has now been seen
	data.currentPage = page > 0 ? page : 0;

	if (numPages > 0 && numPages != data.numOfPages) {
		data.numOfPages = numPages;
		tagChanged = true;
	}

	if (data.numOfPages > 0 && data.maxPageReached > data.numOfPages) {
		data.maxPageReached = data.numOfPages;   //text got shorter since last time
		tagChanged = true;
	}

	if (pagesReached > data.maxPageReached) {
		data.maxPageReached = pagesReached;
		tagChanged = true;
	}

	savedBookMarks[bookId] = data;
	
	logger::debug("bookmark [{:08X}] page[{}] maxPageReached[{}] numOfPages[{}]",
		bookId, data.currentPage, data.maxPageReached, data.numOfPages);

	savedBookMarksMutex.unlock();

	if (tagChanged) {
		UpdateProgressTag(bookId, true);
	}
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

bool IsNote(RE::GFxValue& bookMc) {
	RE::GFxValue bNote;
	if (bookMc.GetMember("bNote", &bNote)){
		if (bNote.IsBool()){
			return bNote.GetBool();
		}
	}
	return false;
}

//Total pages in the open book, or -1 if pagination hasn't finished yet.
int GetMaxPages(RE::GFxValue& bookMc) {
	RE::GFxValue paginationIndex, pageInfo;

	if (!bookMc.GetMember("iPaginationIndex", &paginationIndex) || !paginationIndex.IsNumber() ||
		paginationIndex.GetNumber() != -1) {
		return -1;
	}

	if (!bookMc.GetMember("PageInfoA", &pageInfo) || !pageInfo.IsArray()) {
		return -1;
	}

	int count = static_cast<int>(pageInfo.GetArraySize());

	//notes keep a trailing entry for the end of the text
	if (IsNote(bookMc)) {
		count -= 1;
	}

	return count > 0 ? count : -1;
}

//Furthest page visible right now, 1 based.
//Books show two pages: left page 0 shows pages 1-2, left page 28 shows 29-30.
//Clamped to the total, so an odd-length book ends at its last page (19 / 19, not 20).
int GetPagesReached(RE::GFxValue& bookMc, int maxPages) {
	RE::GFxValue left;
	if (!bookMc.GetMember("iLeftPageNumber", &left) || !left.IsNumber()) {
		return 0;
	}

	const int reached = static_cast<int>(left.GetNumber()) + (IsNote(bookMc) ? 1 : 2);
	return (maxPages > 0) ? std::min(reached, maxPages) : reached;
}

//Reads page number, page count and progress. Does nothing until pagination has finished.
void UpdateCurrentProgress(RE::GFxValue& bookMc) {
	if (currentMaxPages == -1){
		const int maxPages = GetMaxPages(bookMc);
		if (maxPages <= 0){
			return;
		}
		currentMaxPages = maxPages;
	}

	RE::GFxValue left;
	if (bookMc.GetMember("iLeftPageNumber", &left) && left.IsNumber()) {
		currentiLeftPageNumber = static_cast<int>(left.GetNumber());
	}

	const int reached = GetPagesReached(bookMc, currentMaxPages);
	if (reached > currentPagesReached) {
		currentPagesReached = reached;
	}
}

//called on button up while the book is open
void SaveiLeftPageNumber() {
	RE::GFxValue bookMc = GetBookMc();
	if (!bookMc.IsDisplayObject()) {
		return;
	}

	UpdateCurrentProgress(bookMc);
}

//Returns true when finished (restored, or nothing left to do). Returns false if pagination isn't done yet.
//Runs for every opened book: restores the bookmark if there is one, and records the first spread as read.
bool TryRestorePage(RE::FormID bookId, int page) {
	if (openedBookId != bookId) {
		return true;  //menu closed or a different book was opened
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

	if (page > 0) {
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
		}
		else {
			//make the bookmarked page the start of the cached set, like PrepForClose does,
			//so the engine's offsets 0/1 show it
			bookMc.SetMember("iPageSetIndex", arg);
			bookMc.Invoke("UpdatePages", nullptr, nullptr, 0);
			logger::debug("restored page [{}] for [{:08X}]", page, bookId);
		}
	}

	UpdateCurrentProgress(bookMc);
	return true;
}

//Retries on a short timer. Re-queuing a UI task directly from inside a UI task could run
//every retry in the same frame, because SKSE drains the UI queue until it's empty.
void QueueRestorePage(RE::FormID bookId, int page, int attempt = 0) {
	constexpr int kMaxAttempts = 80;  //~2 seconds

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

//inventory icon override ======================================================================================================================================================
//Wraps SkyUI's InventoryListEntry.formatName(a_entryField, a_entryObject, a_state),
//which is where the read icon is set. Runs the original, then dims the eye unless
//this plugin has seen the book's last page.
class FormatNameWrapper : public RE::GFxFunctionHandler {
public:
	void Call(Params& a_params) override {
		if (!a_params.thisPtr) {
			return;
		}

		//1. SkyUI's original formatName, same this/args
		RE::GFxValue original;
		if (a_params.thisPtr->GetMember("_sbm_origFormatName", &original)) {
			std::vector<RE::GFxValue> callArgs;
			callArgs.reserve(a_params.argCount + 1);
			callArgs.push_back(*a_params.thisPtr);  //Function.call(thisArg, ...)
			for (std::uint32_t i = 0; i < a_params.argCount; i++) {
				callArgs.push_back(a_params.args[i]);
			}
			original.Invoke("call", a_params.retVal, callArgs.data(), callArgs.size());
		}

		RE::GFxValue readIcon;
		if (!a_params.thisPtr->GetMember("readIcon", &readIcon) || !readIcon.IsDisplayObject()) {
			return;
		}

		//rows are recycled while scrolling, so always reset first
		RE::GFxValue alpha;
		alpha.SetNumber(100.0);

		if (a_params.argCount >= 2 && a_params.args[1].IsObject()) {
			RE::GFxValue isRead, formIdVal;
			if (a_params.args[1].GetMember("isRead", &isRead) && isRead.IsBool() && isRead.GetBool() &&
				a_params.args[1].GetMember("formId", &formIdVal) && formIdVal.IsNumber()) {

				const auto formId = static_cast<RE::FormID>(formIdVal.GetNumber());

				savedBookMarksMutex.lock();
				auto it = savedBookMarks.find(formId);
				//full eye only once this plugin has seen the last page.
				//Books read before the plugin was installed have no entry, so they stay dimmed.
				const bool fullyRead = it != savedBookMarks.end() &&
					it->second.numOfPages > 0 && it->second.maxPageReached >= it->second.numOfPages;
				savedBookMarksMutex.unlock();

				if (!fullyRead) {
					alpha.SetNumber(40.0);
				}
			}
		}

		readIcon.SetMember("_alpha", alpha);
	}
};

void PatchInventoryListEntry(RE::GFxMovieView* a_movie) {
	if (!a_movie) {
		return;
	}

	RE::GFxValue proto;
	if (!a_movie->GetVariable(&proto, "_global.InventoryListEntry.prototype") || !proto.IsObject()) {
		logger::debug("InventoryListEntry not found in this menu");
		return;
	}

	RE::GFxValue already;
	if (proto.GetMember("_sbm_origFormatName", &already) && !already.IsUndefined()) {
		return;  //already patched in this movie
	}

	RE::GFxValue original;
	if (!proto.GetMember("formatName", &original) || original.IsUndefined()) {
		logger::warn("InventoryListEntry.formatName not found");
		return;
	}

	static RE::GPtr<FormatNameWrapper> handler{ new FormatNameWrapper() };

	RE::GFxValue wrapper;
	a_movie->CreateFunction(&wrapper, handler.get());

	proto.SetMember("_sbm_origFormatName", original);
	proto.SetMember("formatName", wrapper);

	//redraw rows that were already drawn before the patch
	RE::GFxValue list;
	if (a_movie->GetVariable(&list, "_root.Menu_mc.inventoryLists.itemList") && list.IsDisplayObject()) {
		list.Invoke("UpdateList", nullptr, nullptr, 0);  //ScrollingList.UpdateList
	}

	logger::info("patched InventoryListEntry.formatName");
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
		if (!event) {
			return RE::BSEventNotifyControl::kContinue;
		}

		if (event->menuName == RE::BookMenu::MENU_NAME) {
			auto* inputManager = RE::BSInputDeviceManager::GetSingleton();
			auto* inputSink = InputEventSink::GetSingleton();

			//closing -- doesn't need the movie, progress was captured after pagination and on button up
			if (!event->opening) {
				if (openedBookId != 0) {
					SetSavedBookMark(openedBookId, currentiLeftPageNumber, currentPagesReached, currentMaxPages);
				}
				openedBookId = 0;
				currentiLeftPageNumber = 0;
				currentPagesReached = 0;
				currentMaxPages = -1;

				if (inputManager && inputSink->sinkAdded) {
					inputManager->RemoveEventSink(inputSink);
					inputSink->sinkAdded = false;
				}
			}
			else {  //opening
				currentMaxPages = -1;
				currentPagesReached = 0;
				
				auto* book = RE::BookMenu::GetTargetForm();
				if (!gfuncs::IsFormValid(book)) {
					logger::warn("BookMenuOpenEvent: book not valid");
					return RE::BSEventNotifyControl::kContinue;
				}

				openedBookId = book->GetFormID();

				logger::trace("Book Menu opened for [{}]", gfuncs::GetFormNameAndId(book));

				if (inputManager && !inputSink->sinkAdded) {
					inputManager->AddEventSink(inputSink);
					inputSink->sinkAdded = true;
				}

				//keep the bookmark if the book is closed before the restore lands
				int savedPage = GetSavedBookMark(openedBookId);
				currentiLeftPageNumber = savedPage;

				//always queue: restores the bookmark if there is one, and reads page count + progress
				QueueRestorePage(openedBookId, savedPage);
			}
		}
		else if (event->opening && bShowIcon) {
			if (IsItemMenu(event->menuName)){
				std::string menuName(event->menuName.c_str());  //copy -- the event won't outlive the task
				if (auto* tasks = SKSE::GetTaskInterface()) {
					tasks->AddUITask([menuName]() {
						auto* ui = RE::UI::GetSingleton();
						auto movie = ui ? ui->GetMovieView(menuName) : nullptr;
						PatchInventoryListEntry(movie.get());
					});
				}
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
			// RE::ConsoleLog::GetSingleton()->Print("SSEBookmarks Installed");
			logger::info("SSEBookmarks Installed, bShowProgress [{}] bShowIcon[{}]", bShowProgress, bShowIcon);
			break;
		}
	}
}

//serialization=======================================================================================================================================================

//this was 1 in version 0.0.1 and 0.0.2, changed to 2 in 0.0.3
constexpr std::uint32_t kBookMarksVersion = 2;
constexpr std::uint32_t kBookMarksRecord = 'BKMK';

//only saved current bookmark page in version 1. Caller holds savedBookMarksMutex.
bool LoadVersion1(SKSE::SerializationInterface* ssi) {
	std::uint32_t count = 0;
	if (!ssi->ReadRecordData(count)) {
		logger::error("failed to read bookmark count");
		return false;
	}

	for (std::uint32_t i = 0; i < count; i++) {
		RE::FormID oldId = 0;
		if (!ssi->ReadRecordData(oldId)) {
			logger::error("failed to read bookmark id [{}]", i);
			return false;
		}

		std::int32_t page = 0;
		if (!ssi->ReadRecordData(page)) {
			logger::error("failed to read bookmark page [{}]", i);
			return false;
		}

		RE::FormID newId = 0;
		//skip books whose plugin was removed
		if (!ssi->ResolveFormID(oldId, newId)) {
			logger::debug("couldn't resolve book [{:08X}], bookmark dropped", oldId);
			continue;
		}

		if (page > 0) {
			savedBookMarks[newId].currentPage = page;
		}
	}

	logger::debug("loaded [{}] bookmarks", savedBookMarks.size());
	return true;
}

//added BookData for progress tags. Caller holds savedBookMarksMutex.
bool LoadVersionCurrent(SKSE::SerializationInterface* ssi) {
	std::uint32_t count = 0;
	if (!ssi->ReadRecordData(count)) {
		logger::error("failed to read bookmark count");
		return false;
	}

	for (std::uint32_t i = 0; i < count; i++) {
		RE::FormID oldId = 0;
		if (!ssi->ReadRecordData(oldId)) {
			logger::error("failed to read bookmark id [{}]", i);
			return false;
		}

		std::int32_t currentPage = 0;
		if (!ssi->ReadRecordData(currentPage)) {
			logger::error("failed to read bookmark currentPage [{}]", i);
			return false;
		}

		std::int32_t maxPageReached = 0;
		if (!ssi->ReadRecordData(maxPageReached)) {
			logger::error("failed to read bookmark maxPageReached [{}]", i);
			return false;
		}

		std::int32_t numOfPages = 0;
		if (!ssi->ReadRecordData(numOfPages)) {
			logger::error("failed to read bookmark numOfPages [{}]", i);
			return false;
		}

		bool nameChanged = false;
		if (!ssi->ReadRecordData(nameChanged)) {
			logger::error("failed to read bookmark nameChanged [{}]", i);
			return false;
		}

		RE::FormID newId = 0;
		//skip books whose plugin was removed
		if (!ssi->ResolveFormID(oldId, newId)) {
			logger::debug("couldn't resolve book [{:08X}], bookmark dropped", oldId);
			continue;
		}

		//keep every entry: having one means the book has been seen, even if it's back on page 0
		BookData data;
		data.currentPage = currentPage;
		data.maxPageReached = maxPageReached;
		data.numOfPages = numOfPages;
		data.nameChanged = nameChanged;
		savedBookMarks[newId] = data;
	}

	logger::debug("loaded [{}] bookmarks", savedBookMarks.size());
	return true;
}

void LoadCallback(SKSE::SerializationInterface* ssi) {
	logger::trace("LoadCallback started");
	GetSettingsFromIni();

	if (!ssi) {
		logger::error("ssi doesn't exist, aborting load.");
		return;
	}

	std::vector<RE::FormID> loadedIds;

	savedBookMarksMutex.lock();
	savedBookMarks.clear();

	std::uint32_t record, version, length;
	while (ssi->GetNextRecordInfo(record, version, length)) {
		logger::trace("record[{}]", gfuncs::uint32_to_string(record));

		if (record != kBookMarksRecord) {
			continue;
		}

		if (version == 1) {
			LoadVersion1(ssi);
		}
		else if (version == kBookMarksVersion) {
			LoadVersionCurrent(ssi);
		}
		else {
			logger::warn("bookmarks record version [{}] not recognized, skipping", version);
		}
	}

	loadedIds.reserve(savedBookMarks.size());
	for (auto& [id, data] : savedBookMarks) {
		loadedIds.push_back(id);
	}

	savedBookMarksMutex.unlock();

	//name tags are applied outside the lock (see UpdateProgressTag)
	for (auto id : loadedIds) {
		UpdateProgressTag(id, false);
	}

	logger::trace("LoadCallback complete");
}

//Caller holds savedBookMarksMutex.
int SaveCurrentVersion(SKSE::SerializationInterface* ssi, std::uint32_t count) {
	int savedCount = 0;
	
	if (!ssi->WriteRecordData(count)) {
		logger::error("failed to write bookmark count");
		return savedCount;
	}

	for (auto& [bookId, data] : savedBookMarks) {
		if (!ssi->WriteRecordData(bookId)) {
			logger::error("failed to write bookId[{:08X}]", bookId);
			return savedCount;
		}

		if (!ssi->WriteRecordData(static_cast<std::int32_t>(data.currentPage))) {
			logger::error("failed to write bookId[{:08X}] currentPage[{}]", bookId, data.currentPage);
			return savedCount;
		}

		if (!ssi->WriteRecordData(static_cast<std::int32_t>(data.maxPageReached))) {
			logger::error("failed to write bookId[{:08X}] maxPageReached[{}]", bookId, data.maxPageReached);
			return savedCount;
		}

		if (!ssi->WriteRecordData(static_cast<std::int32_t>(data.numOfPages))) {
			logger::error("failed to write bookId[{:08X}] numOfPages[{}]", bookId, data.numOfPages);
			return savedCount;
		}

		if (!ssi->WriteRecordData(data.nameChanged)) {
			logger::error("failed to write bookId[{:08X}] nameChanged[{}]", bookId, data.nameChanged);
			return savedCount;
		}
		
		savedCount++;
	}
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
	int savedCount = SaveCurrentVersion(ssi, count);
	savedBookMarksMutex.unlock();

	if (savedCount == count){
		logger::debug("SaveCallback complete, saved [{}] bookmarks. savedBookMarks size = [{}]", savedCount, count);
	}
	else {
		logger::error("SaveCallback complete, saved [{}] bookmarks. savedBookMarks size = [{}]", savedCount, count);
	}
}

//Runs before a load (and on new game). Removes tags this save added, so they don't
//stick to book names when loading a save where those books have no progress.
void RevertCallback(SKSE::SerializationInterface*) {
	std::vector<RE::FormID> tagged;

	savedBookMarksMutex.lock();
	for (auto& [id, data] : savedBookMarks) {
		if (data.nameChanged) {
			tagged.push_back(id);
		}
	}
	savedBookMarks.clear();
	savedBookMarksMutex.unlock();

	for (auto id : tagged) {
		RE::TESForm* form = RE::TESForm::LookupByID(id);
		if (!gfuncs::IsFormValid(form)) {
			continue;
		}
		std::string name = form->GetName();
		std::string newName = RemoveProgressTag(name);
		if (newName != name) {
			gfuncs::SetFormName(form, newName);
		}
	}
}

//init================================================================================================================================================================
SKSEPluginLoad(const SKSE::LoadInterface* skse) {
	SKSE::Init(skse);
	REL::Module::reset();  //make sure REL offsets are correct before touching them. Caused ctd when opening the game without this.

	SetupLog("Data/SKSE/Plugins/SSEBookmarks.ini");
	GetSettingsFromIni();

	SKSE::GetMessagingInterface()->RegisterListener(MessageListener);

	auto* serialization = SKSE::GetSerializationInterface();
	serialization->SetUniqueID('EBm7');
	serialization->SetSaveCallback(SaveCallback);
	serialization->SetLoadCallback(LoadCallback);
	serialization->SetRevertCallback(RevertCallback);

	return true;
}
