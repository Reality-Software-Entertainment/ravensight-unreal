#include "Misc/AutomationTest.h"
#include "RavensightProtocol.h"
#if WITH_DEV_AUTOMATION_TESTS
BEGIN_DEFINE_SPEC(FRavensightConfigSpec, "Ravensight.ConfigProtocol", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
END_DEFINE_SPEC(FRavensightConfigSpec)
void FRavensightConfigSpec::Define()
{
 It("validates public key and variant identifiers", [this]()
 {
  TestTrue("key", RavensightConfigProtocol::Name(TEXT("difficulty_2"), 64));
  TestFalse("uppercase", RavensightConfigProtocol::Name(TEXT("Difficulty"), 64));
  TestFalse("key hyphen", RavensightConfigProtocol::Name(TEXT("hard-mode"), 64));
  TestTrue("variant hyphen", RavensightConfigProtocol::Name(TEXT("hard-mode"), 40, true));
  TestTrue("variant words are safe values", RavensightConfigProtocol::Name(TEXT("prototype"), 40, true));
  TestFalse("too long", RavensightConfigProtocol::Name(FString::ChrN(65, TCHAR('a')), 64));
 });
 It("only sends exact strong sha256 validators", [this]()
 {
  const FString Tag = FString(TEXT("\"")) + FString::ChrN(64, TCHAR('a')) + TEXT("\"");
  TestTrue("strong", RavensightConfigProtocol::ETag(Tag));
  TestFalse("wildcard", RavensightConfigProtocol::ETag(TEXT("*")));
  TestFalse("injection", RavensightConfigProtocol::ETag(Tag + TEXT("\r\nX-Test: y")));
 });
 It("bounds nesting before engine parsing and handles quoted delimiters", [this]()
 {
  TestTrue("quoted brackets", RavensightConfigProtocol::BoundedDocument(TEXT("{\"v\":\"[[]]\"}")));
  TestFalse("depth", RavensightConfigProtocol::BoundedDocument(FString::ChrN(25, TCHAR('[')) + FString::ChrN(25, TCHAR(']'))));
  TestFalse("unfinished", RavensightConfigProtocol::BoundedDocument(TEXT("{\"v\":\"hi")));
  TestFalse("unbalanced", RavensightConfigProtocol::BoundedDocument(TEXT("}")));
 });
}
#endif
