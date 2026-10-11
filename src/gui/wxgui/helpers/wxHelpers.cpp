#include "wxgui/helpers/wxHelpers.h"

#include <wx/wupdlock.h>
#include <wx/stattext.h>
#include <wx/slider.h>
#include <wx/dirdlg.h>
#include <wx/display.h>

#if BOOST_OS_LINUX || BOOST_OS_BSD
#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include <gdk/gdkwindow.h>
#include <gdk/gdkx.h>
#ifdef HAS_WAYLAND
#include <gdk/gdkwayland.h>
#endif
#endif

#include "wxgui/helpers/wxControlObject.h"
#include "DualscreenSession.h"

void wxAutosizeColumn(wxListCtrlBase* ctrl, int col)
{
	ctrl->SetColumnWidth(col, wxLIST_AUTOSIZE_USEHEADER);
	int wh = ctrl->GetColumnWidth(col);
	ctrl->SetColumnWidth(col, wxLIST_AUTOSIZE);
	int wc = ctrl->GetColumnWidth(col);
	if (wh > wc)
	ctrl->SetColumnWidth(col, wxLIST_AUTOSIZE_USEHEADER);
}

void wxAutosizeColumns(wxListCtrlBase* ctrl, int col_start, int col_end)
{
	wxWindowUpdateLocker lock(ctrl);
	for (int i = col_start; i <= col_end; ++i)
		wxAutosizeColumn(ctrl, i);
}

void update_slider_text(wxCommandEvent& event, const wxFormatString& format /*= "%d%%"*/)
{
	const auto slider = dynamic_cast<wxSlider*>(event.GetEventObject());
	wxASSERT(slider);

	auto slider_text = dynamic_cast<wxControlObject*>(event.GetEventUserData())->GetControl<wxStaticText>();
	wxASSERT(slider_text);

	slider_text->SetLabel(wxString::Format(format, slider->GetValue()));
}

uint32 fix_raw_keycode(uint32 keycode, uint32 raw_flags)
{
#if BOOST_OS_WINDOWS
	const auto flags = (HIWORD(raw_flags) & 0xFFF);
	if(keycode == VK_SHIFT)
	{
		if(flags == 0x2A)
			return 160;
		else if (flags == 0x36)
			return 161;
	}
	else if (keycode == VK_CONTROL)
	{
		if (flags == 0x1d)
			return 162;
		else if (flags == 0x11d)
			return 163;
	}
	else if (keycode == VK_MENU)
	{
		if ((flags & 0xFF) == 0x38)
			return 164;
		else if ((flags & 0xFF) == 0x38)
			return 165;
	}
#endif

	return keycode;
}

#if (BOOST_OS_LINUX || BOOST_OS_BSD) && defined(__WXGTK__)
namespace
{
	struct FullscreenCursor
	{
		wxWindow* window = nullptr;
		guint timer = 0;
		bool hidden = false;
	};

	// Fullscreen as Cemu means it, or a window that covers its whole display
	// anyway -- which is how it runs in a Steam Deck's Game Mode, where the
	// compositor shows every window full screen whether it asked or not.
	bool FillsItsDisplay(wxWindow* window)
	{
		auto* frame = dynamic_cast<wxTopLevelWindow*>(wxGetTopLevelParent(window));
		if (!frame)
			return false;
		if (frame->IsFullScreen())
			return true;
		const int index = wxDisplay::GetFromWindow(frame);
		if (index == wxNOT_FOUND)
			return false;
		return frame->GetScreenRect().Contains(wxDisplay(index).GetGeometry());
	}

	// The window's own cursor rather than the global one, which Cemu's
	// fullscreen timer sets back to the ordinary arrow whenever the pointer
	// moves -- and a touch moves it.
	gboolean OnFullscreenCursorTick(gpointer user_data)
	{
		auto* state = static_cast<FullscreenCursor*>(user_data);
		// Dualscreen's Pointer setting decides, when one is attached: shown,
		// the pointer comes and goes with the mouse as Cemu's own timer has
		// it, even over the whole display; hidden, it never shows. Without
		// one, it is hidden wherever the game fills the display.
		const int setting = DualscreenSession::instance().PointerSetting();
		const bool hide = setting >= 0 ? setting == 0 : FillsItsDisplay(state->window);
		if (hide != state->hidden)
		{
			state->hidden = hide;
			const wxCursor cursor = hide ? wxCursor(wxCURSOR_BLANK) : wxNullCursor;
			state->window->SetCursor(cursor);
			// And the window it is in, whose cursor anything else in it that
			// has none of its own inherits -- so nothing in a fullscreen game
			// window shows one.
			if (wxWindow* frame = wxGetTopLevelParent(state->window))
				frame->SetCursor(cursor);
		}
		return G_SOURCE_CONTINUE;
	}

	void OnFullscreenCursorWidgetGone(GtkWidget*, gpointer user_data)
	{
		auto* state = static_cast<FullscreenCursor*>(user_data);
		if (state->timer)
			g_source_remove(state->timer);
		delete state;
	}
}
#endif

void wxHideCursorInFullscreen(wxWindow* window)
{
#if (BOOST_OS_LINUX || BOOST_OS_BSD) && defined(__WXGTK__)
	if (!window)
		return;
	GtkWidget* widget = window->m_wxwindow ? window->m_wxwindow : window->m_widget;
	if (!widget)
		return;
	auto* state = new FullscreenCursor();
	state->window = window;
	g_signal_connect(widget, "destroy", G_CALLBACK(OnFullscreenCursorWidgetGone), state);
	// On a clock, so it follows the window into and out of fullscreen
	// whatever put it there.
	state->timer = g_timeout_add(250, OnFullscreenCursorTick, state);
	OnFullscreenCursorTick(state);
#endif
}

void wxDropInputMethod(wxWindow* window)
{
#if (BOOST_OS_LINUX || BOOST_OS_BSD) && defined(__WXGTK__)
	// wxGTK checks for a null context everywhere it uses one -- windows made
	// without an expose handler never get one at all -- and frees whatever is
	// here when the window goes, so releasing it now and leaving null is the
	// state such a window would have had from the start.
	if (window && window->m_imContext)
	{
		g_object_unref(window->m_imContext);
		window->m_imContext = nullptr;
	}
#endif
}

WindowSystem::WindowHandleInfo initHandleContextFromWxWidgetsWindow(wxWindow* wxw)
{
	WindowSystem::WindowHandleInfo handleInfo;
#if BOOST_OS_WINDOWS
	handleInfo.backend = WindowSystem::WindowHandleInfo::Backend::Windows;
	handleInfo.surface = reinterpret_cast<void*>(wxw->GetHWND());
#elif BOOST_OS_LINUX || BOOST_OS_BSD
	GtkWidget* gtkWidget = (GtkWidget*)wxw->GetHandle(); // returns GtkWidget
	gtk_widget_realize(gtkWidget);
	GdkWindow* gdkWindow = gtk_widget_get_window(gtkWidget);
	GdkDisplay* gdkDisplay = gdk_window_get_display(gdkWindow);
	if (GDK_IS_X11_WINDOW(gdkWindow))
	{
		handleInfo.backend = WindowSystem::WindowHandleInfo::Backend::X11;
		handleInfo.surface = reinterpret_cast<void*>(gdk_x11_window_get_xid(gdkWindow));
		handleInfo.display = gdk_x11_display_get_xdisplay(gdkDisplay);
		if (!handleInfo.display)
		{
			cemuLog_log(LogType::Force, "Unable to get xlib display");
		}
	}
#ifdef HAS_WAYLAND
	else if (GDK_IS_WAYLAND_WINDOW(gdkWindow))
	{
		handleInfo.backend = WindowSystem::WindowHandleInfo::Backend::Wayland;
		handleInfo.surface = gdk_wayland_window_get_wl_surface(gdkWindow);
		handleInfo.display = gdk_wayland_display_get_wl_display(gdkDisplay);
	}
#endif
	else
	{
		cemuLog_log(LogType::Force, "Unsuported GTK backend");
	}
#elif BOOST_OS_MACOS
	handleInfo.backend = WindowSystem::WindowHandleInfo::Backend::Cocoa;
	handleInfo.surface = reinterpret_cast<void*>(wxw->GetHandle());
#endif
	return handleInfo;
}
