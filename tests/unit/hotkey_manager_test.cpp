#include <gv/core/hotkey_manager.h>

#include <gtest/gtest.h>

TEST (HotkeyManager, SupportsKeyboardInputs)
{
   EXPECT_TRUE (gv::core::HotkeyManager::supports ("F1"));
   EXPECT_TRUE (gv::core::HotkeyManager::supports ("F24"));
   EXPECT_TRUE (gv::core::HotkeyManager::supports ("Ctrl+Shift+G"));
   EXPECT_TRUE (gv::core::HotkeyManager::supports ("Win+PrintScreen"));
   EXPECT_TRUE (gv::core::HotkeyManager::supports ("Alt+Num4"));
   EXPECT_TRUE (gv::core::HotkeyManager::supports ("VolumeUp"));
}

TEST (HotkeyManager, SupportsMouseInputs)
{
   EXPECT_TRUE (gv::core::HotkeyManager::supports ("Mouse1"));
   EXPECT_TRUE (gv::core::HotkeyManager::supports ("Mouse2"));
   EXPECT_TRUE (gv::core::HotkeyManager::supports ("Mouse3"));
   EXPECT_TRUE (gv::core::HotkeyManager::supports ("Ctrl+Mouse4"));
   EXPECT_TRUE (gv::core::HotkeyManager::supports ("Mouse5"));
}

TEST (HotkeyManager, RejectsInvalidInputs)
{
   EXPECT_FALSE (gv::core::HotkeyManager::supports (""));
   EXPECT_FALSE (gv::core::HotkeyManager::supports ("Ctrl"));
   EXPECT_FALSE (gv::core::HotkeyManager::supports ("F25"));
   EXPECT_FALSE (gv::core::HotkeyManager::supports ("Mouse6"));
   EXPECT_FALSE (gv::core::HotkeyManager::supports ("F5+Mouse4"));
   EXPECT_FALSE (gv::core::HotkeyManager::supports ("F5+"));
   EXPECT_FALSE (gv::core::HotkeyManager::supports ("Ctrl+Ctrl+F5"));
}
