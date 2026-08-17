#if WITH_DEV_AUTOMATION_TESTS

#include "CorsairsSession.h"

#include "CorsairsNet/include/CommandMessages.h"
#include "CorsairsNet/include/Packet.h"
#include "Misc/AutomationTest.h"

namespace
{
using Corsairs::Net::RPacket;
using Corsairs::Net::WPacket;
namespace Msg = Corsairs::Net::Msg;

template <typename MessageType>
void Deliver(UCorsairsSession* Session, const MessageType& Message)
{
	WPacket Wire = Msg::serialize(Message);
	RPacket Packet(Wire.Data(), Wire.GetPacketSize());
	Session->HandlePacketForTests(Packet);
}

bool IsEmpty(const FCorsairsNpcTalkPage& Page)
{
	return Page.NpcWorldId == 0 && Page.Command == 0 && Page.Text.IsEmpty();
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorsairsSessionNpcTalkPageTest,
	"Corsairs.Net.Session.NpcTalkPage.ReceivesExactWireAndClearsForNpc",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCorsairsSessionNpcTalkPageTest::RunTest(const FString&)
{
	UCorsairsSession* Session = NewObject<UCorsairsSession>();
	int32 Notifications = 0;
	bool bObservedExactPage = false;
	Session->SetNpcTalkPageObserverForTests(
		[&](const FCorsairsNpcTalkPage& Page)
		{
			++Notifications;
			bObservedExactPage =
				Page.NpcWorldId == 7001 &&
				Page.Command == 4 &&
				Page.Text == TEXT("Привет, моряк!");
		});

	Msg::McTalkInfoMessage Talk;
	Talk.npcId = 7001;
	Talk.cmd = 4;
	Talk.text = "Привет, моряк!";
	Deliver(Session, Talk);

	TestEqual(TEXT("ответ NPC сохранён точно"), Notifications, 1);
	TestTrue(TEXT("делегат видит NPC, страницу и текст"), bObservedExactPage);
	TestEqual(TEXT("getter возвращает NPC"), Session->GetNpcTalkPage().NpcWorldId, int64{7001});
	TestEqual(TEXT("getter возвращает команду страницы"), Session->GetNpcTalkPage().Command, int64{4});
	TestEqual(TEXT("getter возвращает текст"), Session->GetNpcTalkPage().Text, FString(TEXT("Привет, моряк!")));

	Msg::McChaEndSeeMessage OtherActorLeft;
	OtherActorLeft.worldId = 7002;
	Deliver(Session, OtherActorLeft);
	TestFalse(TEXT("уход другого NPC не очищает диалог"), IsEmpty(Session->GetNpcTalkPage()));

	Msg::McChaEndSeeMessage NpcLeft;
	NpcLeft.worldId = 7001;
	Deliver(Session, NpcLeft);
	TestTrue(TEXT("уход собеседника очищает диалог"), IsEmpty(Session->GetNpcTalkPage()));

	Deliver(Session, Talk);
	Msg::McCloseTalkMessage Close;
	Close.npcId = 7001;
	Deliver(Session, Close);
	TestTrue(TEXT("CLOSETALK очищает диалог"), IsEmpty(Session->GetNpcTalkPage()));

	Deliver(Session, Talk);
	Session->Logout();
	TestTrue(TEXT("logout очищает диалог"), IsEmpty(Session->GetNpcTalkPage()));
	return true;
}

#endif
