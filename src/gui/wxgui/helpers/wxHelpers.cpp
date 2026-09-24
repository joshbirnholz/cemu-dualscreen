#include "wxgui/helpers/wxHelpers.h"

#include <wx/wupdlock.h>
#include <wx/stattext.h>
#include <wx/slider.h>
#include <wx/dirdlg.h>
#include <wx/display.h>

#include <cmath>

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
		// Whether a real mouse or trackpad is what last moved the pointer.
		// Until one has, it is taken to be a finger's.
		bool mouse_moved_it = false;
		bool hidden = false;
		// Where the pointer was last seen, in screen pixels.
		bool placed = false;
		double x = 0.0;
		double y = 0.0;
		// Short glides in a row with nothing held. One is a finger landing
		// near where the last one did; a mouse makes a run of them.
		int glides = 0;
		int reported = 0;
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

	// Hidden when the game fills the screen, unless a mouse is what last moved
	// it. Cemu's own fullscreen timer brings the pointer back whenever it has
	// moved, however it moved -- which a touch does -- so this sets the
	// window's own cursor, which that leaves alone.
	void ApplyTouchCursor(TouchCursor* state)
	{
		const bool hide = !state->mouse_moved_it && FillsItsDisplay(state->window);
		if (hide == state->hidden)
			return;
		state->hidden = hide;
		state->window->SetCursor(hide ? wxCursor(wxCURSOR_BLANK) : wxNullCursor);
	}

	// Which of the two moved the pointer, from how it moved rather than what
	// claims to have moved it: a Steam Deck in Game Mode hands a touch over
	// as ordinary mouse input, from a device that says it is a mouse. What a
	// finger cannot do is hover. A mouse or trackpad glides the pointer about
	// with nothing pressed; a finger only moves it pressed, or makes it jump
	// straight to where it lands. So a press hides it, a jump leaves it
	// hidden, a drag leaves it as it was, and only a short glide with nothing
	// held shows it -- which is a mouse, whatever the device says.
	constexpr double kGlidePixels = 48.0;
	constexpr int kGlidesForAMouse = 3;

	// Runs ahead of wxWidgets' own handlers for the same event -- "event" is
	// emitted before the specific signal each one connects to -- and never
	// consumes anything, so the window still gets every event it did before.
	gboolean OnAnyEventForTouchCursor(GtkWidget*, GdkEvent* event, gpointer user_data)
	{
		auto* state = static_cast<TouchCursor*>(user_data);
		const char* verdict = nullptr;
		switch (event->type)
		{
		case GDK_TOUCH_BEGIN:
		case GDK_TOUCH_UPDATE:
		case GDK_TOUCH_END:
			state->mouse_moved_it = false;
			state->glides = 0;
			verdict = "a touch";
			break;
		case GDK_BUTTON_PRESS:
		case GDK_BUTTON_RELEASE:
			state->mouse_moved_it = false;
			state->glides = 0;
			state->placed = true;
			state->x = event->button.x_root;
			state->y = event->button.y_root;
			verdict = "a press";
			break;
		case GDK_MOTION_NOTIFY:
		{
			const GdkEventMotion& motion = event->motion;
			GdkDevice* device = gdk_event_get_source_device(event);
			const bool said_touch = gdk_event_get_pointer_emulated(event) ||
									(device && gdk_device_get_source(device) == GDK_SOURCE_TOUCHSCREEN);
			const bool held = (motion.state & (GDK_BUTTON1_MASK | GDK_BUTTON2_MASK | GDK_BUTTON3_MASK)) != 0;
			const double distance = state->placed
				? std::abs(motion.x_root - state->x) + std::abs(motion.y_root - state->y)
				: kGlidePixels * 2.0;
			state->placed = true;
			state->x = motion.x_root;
			state->y = motion.y_root;
			if (said_touch)
			{
				state->mouse_moved_it = false;
				state->glides = 0;
				verdict = "a touch, by its device";
			}
			else if (held)
			{
				state->glides = 0;
				verdict = "a drag";
			}
			else if (distance > kGlidePixels)
			{
				state->mouse_moved_it = false;
				state->glides = 0;
				verdict = "a jump";
			}
			else
			{
				if (++state->glides >= kGlidesForAMouse)
					state->mouse_moved_it = true;
				verdict = "a glide";
			}
			break;
		}
		default:
			return FALSE;
		}
		const bool was_hidden = state->hidden;
		ApplyTouchCursor(state);
		// Said the first few times, and whenever it changes what is shown:
		// the one thing this rests on is how pointer input reaches Cemu, and
		// on a machine like a Steam Deck in Game Mode that is not a given.
		if (state->reported < 6 || was_hidden != state->hidden)
		{
			++state->reported;
			cemuLog_log(LogType::Force, "pointer: {} over the game; {}", verdict,
						state->hidden ? "hidden" : "shown");
		}
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
