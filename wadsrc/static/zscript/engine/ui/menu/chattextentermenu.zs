/*
** chattextentermenu.zs
** Modal chat input overlay with the character grid.
*/

class ChatTextEnterMenu : GenericMenu
{
	const INPUTGRID_WIDTH = 13;
	const INPUTGRID_HEIGHT = 5;

	const Chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789+-=.,!?@'\":;[]()<>^#$%&*/_ \b";

	String mEnterString;
	int mInputGridX;
	int mInputGridY;
	Font displayFont;

	private native static void SubmitChatMessage(String text);
	private native static void CancelChatMessage();
	private native static bool IsTeamChat();

	override void Init(Menu parent)
	{
		Super.Init(parent);
		mEnterString = "";
		mInputGridX = 0;
		mInputGridY = 0;
		displayFont = Menu.OptionFont();
		DontDim = false;
		DontBlur = false;
		Animated = false;
		AnimatedTransition = false;
	}

	override bool TranslateKeyboardEvents()
	{
		return true;
	}

	private void AppendChar(int ch)
	{
		mEnterString.AppendCharacter(ch);
	}

	private void Submit()
	{
		SubmitChatMessage(mEnterString);
		Close();
	}

	override void OnDestroy()
	{
		CancelChatMessage();
		Super.OnDestroy();
	}

	override bool OnUIEvent(UIEvent ev)
	{
		if (ev.Type == UIEvent.Type_Char)
		{
			AppendChar(ev.KeyChar);
			return true;
		}

		int ch = ev.KeyChar;
		if ((ev.Type == UIEvent.Type_KeyDown || ev.Type == UIEvent.Type_KeyRepeat) && ch == 8)
		{
			if (mEnterString.Length() > 0)
			{
				mEnterString.DeleteLastCharacter();
			}
			return true;
		}
		else if (ev.Type == UIEvent.Type_KeyDown)
		{
			if (ch == UIEvent.Key_ESCAPE)
			{
				Close();
				return true;
			}
			else if (ch == 13)
			{
				Submit();
				return true;
			}
		}

		if (ev.Type == UIEvent.Type_KeyDown || ev.Type == UIEvent.Type_KeyRepeat)
		{
			return true;
		}
		return Super.OnUIEvent(ev);
	}

	override bool MouseEvent(int type, int x, int y)
	{
		int cell_width = 18 * CleanXfac_1;
		int cell_height = 16 * CleanYfac_1;
		int screen_y = screen.GetHeight() - INPUTGRID_HEIGHT * cell_height;
		int screen_x = (screen.GetWidth() - INPUTGRID_WIDTH * cell_width) / 2;

		if (x >= screen_x && x < screen_x + INPUTGRID_WIDTH * cell_width && y >= screen_y)
		{
			mInputGridX = (x - screen_x) / cell_width;
			mInputGridY = (y - screen_y) / cell_height;
			if (type == MOUSE_Release)
			{
				MenuEvent(MKEY_Enter, true);
				if (m_use_mouse == 2)
				{
					mInputGridX = -1;
					mInputGridY = -1;
				}
			}
			return true;
		}

		mInputGridX = -1;
		mInputGridY = -1;
		return Super.MouseEvent(type, x, y);
	}

	override bool MenuEvent(int key, bool fromcontroller)
	{
		if (key == MKEY_Enter && !fromcontroller)
		{
			Submit();
			return true;
		}

		if (key == MKEY_Back)
		{
			Close();
			return true;
		}

		if (fromcontroller)
		{
			if (mInputGridX < 0 || mInputGridY < 0)
			{
				mInputGridX = 0;
				mInputGridY = 0;
			}
		}

		switch (key)
		{
		case MKEY_Down:
			mInputGridY = (mInputGridY + 1) % INPUTGRID_HEIGHT;
			return true;

		case MKEY_Up:
			mInputGridY = (mInputGridY + INPUTGRID_HEIGHT - 1) % INPUTGRID_HEIGHT;
			return true;

		case MKEY_Right:
			mInputGridX = (mInputGridX + 1) % INPUTGRID_WIDTH;
			return true;

		case MKEY_Left:
			mInputGridX = (mInputGridX + INPUTGRID_WIDTH - 1) % INPUTGRID_WIDTH;
			return true;

		case MKEY_Clear:
			if (mEnterString.Length() > 0)
			{
				mEnterString.DeleteLastCharacter();
			}
			return true;

		case MKEY_Enter:
		case MKEY_Input:
			{
				String InputGridChars = Chars;
				if (mInputGridX < 0 || mInputGridY < 0)
				{
					mInputGridX = 0;
					mInputGridY = 0;
				}
				int ch = InputGridChars.ByteAt(mInputGridX + mInputGridY * INPUTGRID_WIDTH);
				if (ch == 0)
				{
					Submit();
				}
				else if (ch == 8)
				{
					if (mEnterString.Length() > 0)
					{
						mEnterString.DeleteLastCharacter();
					}
				}
				else
				{
					AppendChar(ch);
				}
			}
			return true;

		case MKEY_Abort:
			Close();
			return true;
		}

		return false;
	}

	override bool OnInputEvent(InputEvent ev)
	{
		if (ev.type == InputEvent.Type_KeyDown &&
			(ev.KeyScan == InputEvent.Key_Pad_B || ev.KeyScan == InputEvent.Key_Escape))
		{
			Close();
			return true;
		}
		return Super.OnInputEvent(ev);
	}

	override void Drawer()
	{
		String InputGridChars = Chars;
		int cell_width = 18 * CleanXfac_1;
		int cell_height = 16 * CleanYfac_1;
		int promptX = 8 * CleanXfac_1;
		int promptY = 8 * CleanYfac_1;
		String prompt = IsTeamChat() ? "TEAM SAY: " : "SAY: ";

		screen.DrawText(displayFont, Font.CR_ORANGE, promptX, promptY,
			prompt .. mEnterString .. displayFont.GetCursor(),
			DTA_CleanNoMove_1, true);
		screen.DrawText(displayFont, Font.CR_BROWN, promptX, promptY + displayFont.GetHeight() * CleanYfac_1 + 4,
			"Enter sends the message, Backspace deletes.", DTA_CleanNoMove_1, true);

		screen.Dim(0, 0.8, 0, screen.GetHeight() - INPUTGRID_HEIGHT * cell_height, screen.GetWidth(), INPUTGRID_HEIGHT * cell_height);

		if (mInputGridX >= 0 && mInputGridY >= 0)
		{
			screen.Dim(Color(255, 248, 220), 0.6,
				mInputGridX * cell_width - INPUTGRID_WIDTH * cell_width / 2 + screen.GetWidth() / 2,
				mInputGridY * cell_height - INPUTGRID_HEIGHT * cell_height + screen.GetHeight(),
				cell_width, cell_height);
		}

		for (int y = 0; y < INPUTGRID_HEIGHT; ++y)
		{
			for (int x = 0; x < INPUTGRID_WIDTH; ++x)
			{
				int px = x * cell_width - INPUTGRID_WIDTH * cell_width / 2 + screen.GetWidth() / 2;
				int py = y * cell_height - INPUTGRID_HEIGHT * cell_height + screen.GetHeight();
				int ch = InputGridChars.ByteAt(x + y * INPUTGRID_WIDTH);
				String cellText = ch == 8 ? "<" : ch == 0 ? "OK" : String.Format("%c", ch);
				screen.DrawText(displayFont, Font.CR_GREY, px + 3, py + 1, cellText, DTA_CleanNoMove_1, true);
			}
		}
	}
}
