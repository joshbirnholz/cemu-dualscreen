#include "wxgui/helpers/wxHelpers.h"

#include <wx/wupdlock.h>
#include <wx/stattext.h>
#include <wx/slider.h>
#include <wx/dirdlg.h>

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
	struct TouchCursor
	{
		wxWindow* window = nullptr;
		guint timer = 0;
		// Whether the last thing to move the pointer was a real mouse or
		// trackpad. Until one has, the pointer is taken to be a finger's.
		bool mouse_moved_it = false;
		bool hidden = false;
		int reported = 0;
	};

	// Hidden in fullscreen unless a mouse is what last moved it. Cemu's own
	// fullscreen timer brings the pointer back whenever it has moved, however
	// it moved -- which a touch does -- so this sets the window's own cursor,
	// which that leaves alone.
	void ApplyTouchCursor(TouchCursor* state)
	{
		auto* frame = dynamic_cast<wxTopLevelWindow*>(wxGetTopLevelParent(state->window));
		const bool fullscreen = frame && frame->IsFullScreen();
		const bool hide = fullscreen && !state->mouse_moved_it;
		if (hide == state->hidden)
			return;
		state->hidden = hide;
		state->window->SetCursor(hide ? wxCursor(wxCURSOR_BLANK) : wxNullCursor);
	}

	// Runs ahead of wxWidgets' own handlers for the same event -- "event" is
	// emitted before the specific signal each one connects to -- and never
	// consumes anything, so the window still gets every event it did before.
	gboolean OnAnyEventForTouchCursor(GtkWidget*, GdkEvent* event, gpointer user_data)
	{
		auto* state = static_cast<TouchCursor*>(user_data);
		bool from_touch = false;
		switch (event->type)
		{
		case GDK_TOUCH_BEGIN:
		case GDK_TOUCH_UPDATE:
		case GDK_TOUCH_END:
			from_touch = true;
			break;
		case GDK_MOTION_NOTIFY:
		case GDK_BUTTON_PRESS:
		case GDK_BUTTON_RELEASE:
		{
			// A finger arrives as pointer events too, either marked as made up
			// from a touch or coming from the touchscreen itself.
			GdkDevice* device = gdk_event_get_source_device(event);
			const GdkInputSource source =
				device ? gdk_device_get_source(device) : GDK_SOURCE_MOUSE;
			from_touch = gdk_event_get_pointer_emulated(event) ||
						 source == GDK_SOURCE_TOUCHSCREEN;
			// Said the first few times, and whenever it changes: how the
			// pointer is being moved is the one thing that decides this, and
			// on a machine like a Steam Deck in Game Mode it is not a given.
			if (state->reported < 4 || from_touch == state->mouse_moved_it)
			{
				++state->reported;
				cemuLog_log(LogType::Force, "pointer moved by {} ({}, source {}, {})",
							from_touch ? "touch" : "mouse",
							device ? gdk_device_get_name(device) : "no device", (int)source,
							gdk_event_get_pointer_emulated(event) ? "emulated" : "not emulated");
			}
			break;
		}
		default:
			return FALSE;
		}
		state->mouse_moved_it = !from_touch;
		ApplyTouchCursor(state);
		return FALSE;
	}

	gboolean OnTouchCursorTick(gpointer user_data)
	{
		// Also on a clock, for the times nothing moves it: the window going
		// fullscreen, or the game starting with the pointer already over it.
		ApplyTouchCursor(static_cast<TouchCursor*>(user_data));
		return G_SOURCE_CONTINUE;
	}

	void OnTouchCursorWidgetGone(GtkWidget*, gpointer user_data)
	{
		auto* state = static_cast<TouchCursor*>(user_data);
		if (state->timer)
			g_source_remove(state->timer);
		delete state;
	}
}
#endif

void wxHideCursorWhileTouched(wxWindow* window)
{
#if (BOOST_OS_LINUX || BOOST_OS_BSD) && defined(__WXGTK__)
	if (!window)
		return;
	// The widget the events are delivered to: the drawing area wxWidgets puts
	// inside a window it draws itself, where there is one.
	GtkWidget* widget = window->m_wxwindow ? window->m_wxwindow : window->m_widget;
	if (!widget)
		return;
	auto* state = new TouchCursor();
	state->window = window;
	g_signal_connect(widget, "event", G_CALLBACK(OnAnyEventForTouchCursor), state);
	g_signal_connect(widget, "destroy", G_CALLBACK(OnTouchCursorWidgetGone), state);
	state->timer = g_timeout_add(500, OnTouchCursorTick, state);
	ApplyTouchCursor(state);
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
