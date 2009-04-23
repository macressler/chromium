// Copyright (c) 2006-2008 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/tab_contents/tab_contents_view_win.h"

#include <windows.h>

#include "chrome/browser/bookmarks/bookmark_drag_data.h"
#include "chrome/browser/browser.h"  // TODO(beng): this dependency is awful.
#include "chrome/browser/browser_process.h"
#include "chrome/browser/download/download_request_manager.h"
#include "chrome/browser/renderer_host/render_process_host.h"
#include "chrome/browser/renderer_host/render_view_host.h"
#include "chrome/browser/renderer_host/render_view_host_factory.h"
#include "chrome/browser/renderer_host/render_widget_host_view_win.h"
#include "chrome/browser/tab_contents/render_view_context_menu_win.h"
#include "chrome/browser/tab_contents/interstitial_page.h"
#include "chrome/browser/tab_contents/tab_contents_delegate.h"
#include "chrome/browser/tab_contents/web_contents.h"
#include "chrome/browser/tab_contents/web_drag_source.h"
#include "chrome/browser/tab_contents/web_drop_target.h"
#include "chrome/browser/views/sad_tab_view.h"
#include "chrome/common/gfx/chrome_canvas.h"
#include "chrome/common/os_exchange_data.h"
#include "chrome/common/url_constants.h"
#include "chrome/views/focus/view_storage.h"
#include "chrome/views/widget/root_view.h"
#include "net/base/net_util.h"
#include "webkit/glue/plugins/webplugin_delegate_impl.h"
#include "webkit/glue/webdropdata.h"

using WebKit::WebInputEvent;

namespace {

// Windows callback for OnDestroy to detach the plugin windows.
BOOL CALLBACK DetachPluginWindowsCallback(HWND window, LPARAM param) {
  if (WebPluginDelegateImpl::IsPluginDelegateWindow(window) &&
      !IsHungAppWindow(window)) {
    ::ShowWindow(window, SW_HIDE);
    SetParent(window, NULL);
  }
  return TRUE;
}

}  // namespace

// static
TabContentsView* TabContentsView::Create(WebContents* web_contents) {
  return new TabContentsViewWin(web_contents);
}

TabContentsViewWin::TabContentsViewWin(WebContents* web_contents)
    : TabContentsView(web_contents),
      ignore_next_char_event_(false) {
  last_focused_view_storage_id_ =
      views::ViewStorage::GetSharedInstance()->CreateStorageID();
}

TabContentsViewWin::~TabContentsViewWin() {
  // Makes sure to remove any stored view we may still have in the ViewStorage.
  //
  // It is possible the view went away before us, so we only do this if the
  // view is registered.
  views::ViewStorage* view_storage = views::ViewStorage::GetSharedInstance();
  if (view_storage->RetrieveView(last_focused_view_storage_id_) != NULL)
    view_storage->RemoveView(last_focused_view_storage_id_);
}

void TabContentsViewWin::CreateView() {
  set_delete_on_destroy(false);
  // Since we create these windows parented to the desktop window initially, we
  // don't want to create them initially visible.
  set_window_style(WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS);
  WidgetWin::Init(GetDesktopWindow(), gfx::Rect(), false);

  // Remove the root view drop target so we can register our own.
  RevokeDragDrop(GetNativeView());
  drop_target_ = new WebDropTarget(GetNativeView(), web_contents());
}

RenderWidgetHostView* TabContentsViewWin::CreateViewForWidget(
    RenderWidgetHost* render_widget_host) {
  if (render_widget_host->view()) {
    // During testing, the view will already be set up in most cases to the
    // test view, so we don't want to clobber it with a real one. To verify that
    // this actually is happening (and somebody isn't accidentally creating the
    // view twice), we check for the RVH Factory, which will be set when we're
    // making special ones (which go along with the special views).
    DCHECK(RenderViewHostFactory::has_factory());
    return render_widget_host->view();
  }

  RenderWidgetHostViewWin* view =
      new RenderWidgetHostViewWin(render_widget_host);
  view->Create(GetNativeView());
  view->ShowWindow(SW_SHOW);
  return view;
}

gfx::NativeView TabContentsViewWin::GetNativeView() const {
  return WidgetWin::GetNativeView();
}

gfx::NativeView TabContentsViewWin::GetContentNativeView() const {
  if (!web_contents()->render_widget_host_view())
    return NULL;
  return web_contents()->render_widget_host_view()->GetPluginNativeView();
}

gfx::NativeWindow TabContentsViewWin::GetTopLevelNativeWindow() const {
  return ::GetAncestor(GetNativeView(), GA_ROOT);
}

void TabContentsViewWin::GetContainerBounds(gfx::Rect* out) const {
  GetBounds(out, false);
}

void TabContentsViewWin::StartDragging(const WebDropData& drop_data) {
  scoped_refptr<OSExchangeData> data(new OSExchangeData);

  // TODO(tc): Generate an appropriate drag image.

  // We set the file contents before the URL because the URL also sets file
  // contents (to a .URL shortcut).  We want to prefer file content data over a
  // shortcut so we add it first.
  if (!drop_data.file_contents.empty()) {
    // Images without ALT text will only have a file extension so we need to
    // synthesize one from the provided extension and URL.
    FilePath file_name(drop_data.file_description_filename);
    file_name = file_name.BaseName().RemoveExtension();
    if (file_name.value().empty()) {
      // Retrieve the name from the URL.
      file_name = FilePath::FromWStringHack(
          net::GetSuggestedFilename(drop_data.url, L"", L""));
    }
    file_name = file_name.ReplaceExtension(drop_data.file_extension);
    data->SetFileContents(file_name.value(), drop_data.file_contents);
  }
  if (!drop_data.text_html.empty())
    data->SetHtml(drop_data.text_html, drop_data.html_base_url);
  if (drop_data.url.is_valid()) {
    if (drop_data.url.SchemeIs(chrome::kJavaScriptScheme)) {
      // We don't want to allow javascript URLs to be dragged to the desktop,
      // but we do want to allow them to be added to the bookmarks bar
      // (bookmarklets).
      BookmarkDragData::Element bm_elt;
      bm_elt.is_url = true;
      bm_elt.url = drop_data.url;
      bm_elt.title = drop_data.url_title;

      BookmarkDragData bm_drag_data;
      bm_drag_data.elements.push_back(bm_elt);

      bm_drag_data.Write(web_contents()->profile(), data);
    } else {
      data->SetURL(drop_data.url, drop_data.url_title);
    }
  }
  if (!drop_data.plain_text.empty())
    data->SetString(drop_data.plain_text);

  scoped_refptr<WebDragSource> drag_source(
      new WebDragSource(GetNativeView(), web_contents()->render_view_host()));

  DWORD effects;

  // We need to enable recursive tasks on the message loop so we can get
  // updates while in the system DoDragDrop loop.
  bool old_state = MessageLoop::current()->NestableTasksAllowed();
  MessageLoop::current()->SetNestableTasksAllowed(true);
  DoDragDrop(data, drag_source, DROPEFFECT_COPY | DROPEFFECT_LINK, &effects);
  MessageLoop::current()->SetNestableTasksAllowed(old_state);

  if (web_contents()->render_view_host())
    web_contents()->render_view_host()->DragSourceSystemDragEnded();
}

void TabContentsViewWin::OnContentsDestroy() {
  // TODO(brettw) this seems like maybe it can be moved into OnDestroy and this
  // function can be deleted? If you're adding more here, consider whether it
  // can be moved into OnDestroy which is a Windows message handler as the
  // window is being torn down.

  // When a tab is closed all its child plugin windows are destroyed
  // automatically. This happens before plugins get any notification that its
  // instances are tearing down.
  //
  // Plugins like Quicktime assume that their windows will remain valid as long
  // as they have plugin instances active. Quicktime crashes in this case
  // because its windowing code cleans up an internal data structure that the
  // handler for NPP_DestroyStream relies on.
  //
  // The fix is to detach plugin windows from web contents when it is going
  // away. This will prevent the plugin windows from getting destroyed
  // automatically. The detached plugin windows will get cleaned up in proper
  // sequence as part of the usual cleanup when the plugin instance goes away.
  EnumChildWindows(GetNativeView(), DetachPluginWindowsCallback, NULL);
}

void TabContentsViewWin::OnDestroy() {
  if (drop_target_.get()) {
    RevokeDragDrop(GetNativeView());
    drop_target_ = NULL;
  }
}

void TabContentsViewWin::SetPageTitle(const std::wstring& title) {
  if (GetNativeView()) {
    // It's possible to get this after the hwnd has been destroyed.
    ::SetWindowText(GetNativeView(), title.c_str());
    // TODO(brettw) this call seems messy the way it reaches into the widget
    // view, and I'm not sure it's necessary. Maybe we should just remove it.
    ::SetWindowText(
        web_contents()->render_widget_host_view()->GetPluginNativeView(),
        title.c_str());
  }
}

void TabContentsViewWin::Invalidate() {
  // Note that it's possible to get this message after the window was destroyed.
  if (::IsWindow(GetNativeView()))
    InvalidateRect(GetNativeView(), NULL, FALSE);
}

void TabContentsViewWin::SizeContents(const gfx::Size& size) {
  // TODO(brettw) this is a hack and should be removed. See tab_contents_view.h.
  WasSized(size);
}

void TabContentsViewWin::Focus() {
  HWND container_hwnd = GetNativeView();
  if (!container_hwnd)
    return;

  views::FocusManager* focus_manager =
      views::FocusManager::GetFocusManager(container_hwnd);
  if (!focus_manager)
    return;  // During testing we have no focus manager.
  views::View* v = focus_manager->GetViewForWindow(container_hwnd, true);
  DCHECK(v);
  if (v)
    v->RequestFocus();
}

void TabContentsViewWin::SetInitialFocus() {
  if (web_contents()->FocusLocationBarByDefault())
    web_contents()->delegate()->SetFocusToLocationBar();
  else
    ::SetFocus(GetNativeView());
}

void TabContentsViewWin::StoreFocus() {
  views::ViewStorage* view_storage = views::ViewStorage::GetSharedInstance();

  if (view_storage->RetrieveView(last_focused_view_storage_id_) != NULL)
    view_storage->RemoveView(last_focused_view_storage_id_);

  views::FocusManager* focus_manager =
      views::FocusManager::GetFocusManager(GetNativeView());
  if (focus_manager) {
    // |focus_manager| can be NULL if the tab has been detached but still
    // exists.
    views::View* focused_view = focus_manager->GetFocusedView();
    if (focused_view)
      view_storage->StoreView(last_focused_view_storage_id_, focused_view);

    // If the focus was on the page, explicitly clear the focus so that we
    // don't end up with the focused HWND not part of the window hierarchy.
    // TODO(brettw) this should move to the view somehow.
    HWND container_hwnd = GetNativeView();
    if (container_hwnd) {
      views::View* focused_view = focus_manager->GetFocusedView();
      if (focused_view) {
        HWND hwnd = focused_view->GetRootView()->GetWidget()->GetNativeView();
        if (container_hwnd == hwnd || ::IsChild(container_hwnd, hwnd))
          focus_manager->ClearFocus();
      }
    }
  }
}

void TabContentsViewWin::RestoreFocus() {
  views::ViewStorage* view_storage = views::ViewStorage::GetSharedInstance();
  views::View* last_focused_view =
      view_storage->RetrieveView(last_focused_view_storage_id_);

  if (!last_focused_view) {
    SetInitialFocus();
  } else {
    views::FocusManager* focus_manager =
        views::FocusManager::GetFocusManager(GetNativeView());

    // If you hit this DCHECK, please report it to Jay (jcampan).
    DCHECK(focus_manager != NULL) << "No focus manager when restoring focus.";

    if (last_focused_view->IsFocusable() && focus_manager &&
        focus_manager->ContainsView(last_focused_view)) {
      last_focused_view->RequestFocus();
    } else {
      // The focused view may not belong to the same window hierarchy (e.g.
      // if the location bar was focused and the tab is dragged out), or it may
      // no longer be focusable (e.g. if the location bar was focused and then
      // we switched to fullscreen mode).  In that case we default to the
      // default focus.
      SetInitialFocus();
    }
    view_storage->RemoveView(last_focused_view_storage_id_);
  }
}

void TabContentsViewWin::UpdateDragCursor(bool is_drop_target) {
  drop_target_->set_is_drop_target(is_drop_target);
}

void TabContentsViewWin::TakeFocus(bool reverse) {
  if (!web_contents()->delegate()->TakeFocus(reverse)) {
    views::FocusManager* focus_manager =
        views::FocusManager::GetFocusManager(GetNativeView());

    // We may not have a focus manager if the tab has been switched before this
    // message arrived.
    if (focus_manager)
      focus_manager->AdvanceFocus(reverse);
  }
}

void TabContentsViewWin::HandleKeyboardEvent(
    const NativeWebKeyboardEvent& event) {
  // Previous calls to TranslateMessage can generate CHAR events as well as
  // RAW_KEY_DOWN events, even if the latter triggered an accelerator.  In these
  // cases, we discard the CHAR events.
  if (event.type == WebInputEvent::Char && ignore_next_char_event_) {
    ignore_next_char_event_ = false;
    return;
  }
  ignore_next_char_event_ = false;

  // The renderer returned a keyboard event it did not process. This may be
  // a keyboard shortcut that we have to process.
  if (event.type == WebInputEvent::RawKeyDown) {
    views::FocusManager* focus_manager =
        views::FocusManager::GetFocusManager(GetNativeView());
    // We may not have a focus_manager at this point (if the tab has been
    // switched by the time this message returned).
    if (focus_manager) {
      views::Accelerator accelerator(event.windowsKeyCode,
          (event.modifiers & WebInputEvent::ShiftKey) ==
              WebInputEvent::ShiftKey,
          (event.modifiers & WebInputEvent::ControlKey) ==
              WebInputEvent::ControlKey,
          (event.modifiers & WebInputEvent::AltKey) ==
              WebInputEvent::AltKey);

      // This is tricky: we want to set ignore_next_char_event_ if
      // ProcessAccelerator returns true. But ProcessAccelerator might delete
      // |this| if the accelerator is a "close tab" one. So we speculatively
      // set the flag and fix it if no event was handled.
      ignore_next_char_event_ = true;
      if (focus_manager->ProcessAccelerator(accelerator, false)) {
        // DANGER: |this| could be deleted now!
        return;
      } else {
        // ProcessAccelerator didn't handle the accelerator, so we know both
        // that |this| is still valid, and that we didn't want to set the flag.
        ignore_next_char_event_ = false;
      }
    }
  }

  // Any unhandled keyboard/character messages should be defproced.
  // This allows stuff like Alt+F4, etc to work correctly.
  DefWindowProc(event.os_event.hwnd,
                event.os_event.message,
                event.os_event.wParam,
                event.os_event.lParam);
}

void TabContentsViewWin::ShowContextMenu(const ContextMenuParams& params) {
  RenderViewContextMenuWin menu(web_contents(),
                                params,
                                GetNativeView());

  POINT screen_pt = { params.x, params.y };
  MapWindowPoints(GetNativeView(), HWND_DESKTOP, &screen_pt, 1);

  // Enable recursive tasks on the message loop so we can get updates while
  // the context menu is being displayed.
  bool old_state = MessageLoop::current()->NestableTasksAllowed();
  MessageLoop::current()->SetNestableTasksAllowed(true);
  menu.RunMenuAt(screen_pt.x, screen_pt.y);
  MessageLoop::current()->SetNestableTasksAllowed(old_state);
}

void TabContentsViewWin::OnHScroll(int scroll_type, short position,
                                   HWND scrollbar) {
  ScrollCommon(WM_HSCROLL, scroll_type, position, scrollbar);
}

void TabContentsViewWin::OnMouseLeave() {
  // Let our delegate know that the mouse moved (useful for resetting status
  // bubble state).
  if (web_contents()->delegate())
    web_contents()->delegate()->ContentsMouseEvent(web_contents(), false);
  SetMsgHandled(FALSE);
}

LRESULT TabContentsViewWin::OnMouseRange(UINT msg,
                                         WPARAM w_param, LPARAM l_param) {
  switch (msg) {
    case WM_LBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_RBUTTONDOWN: {
      // Make sure this TabContents is activated when it is clicked on.
      if (web_contents()->delegate())
        web_contents()->delegate()->ActivateContents(web_contents());
      DownloadRequestManager* drm =
          g_browser_process->download_request_manager();
      if (drm)
        drm->OnUserGesture(web_contents());
      break;
    }
    case WM_MOUSEMOVE:
      // Let our delegate know that the mouse moved (useful for resetting status
      // bubble state).
      if (web_contents()->delegate()) {
        web_contents()->delegate()->ContentsMouseEvent(web_contents(), true);
      }
      break;
    default:
      break;
  }

  return 0;
}

void TabContentsViewWin::OnPaint(HDC junk_dc) {
  if (web_contents()->render_view_host() &&
      !web_contents()->render_view_host()->IsRenderViewLive()) {
    if (!sad_tab_.get())
      sad_tab_.reset(new SadTabView);
    CRect cr;
    GetClientRect(&cr);
    sad_tab_->SetBounds(gfx::Rect(cr));
    ChromeCanvasPaint canvas(GetNativeView(), true);
    sad_tab_->ProcessPaint(&canvas);
    return;
  }

  // We need to do this to validate the dirty area so we don't end up in a
  // WM_PAINTstorm that causes other mysterious bugs (such as WM_TIMERs not
  // firing etc). It doesn't matter that we don't have any non-clipped area.
  CPaintDC dc(GetNativeView());
  SetMsgHandled(FALSE);
}

// A message is reflected here from view().
// Return non-zero to indicate that it is handled here.
// Return 0 to allow view() to further process it.
LRESULT TabContentsViewWin::OnReflectedMessage(UINT msg, WPARAM w_param,
                                        LPARAM l_param) {
  MSG* message = reinterpret_cast<MSG*>(l_param);
  switch (message->message) {
    case WM_MOUSEWHEEL:
      // This message is reflected from the view() to this window.
      if (GET_KEYSTATE_WPARAM(message->wParam) & MK_CONTROL) {
        WheelZoom(GET_WHEEL_DELTA_WPARAM(message->wParam));
        return 1;
      }
      break;
    case WM_HSCROLL:
    case WM_VSCROLL:
      if (ScrollZoom(LOWORD(message->wParam)))
        return 1;
    default:
      break;
  }

  return 0;
}

void TabContentsViewWin::OnSetFocus(HWND window) {
  // TODO(jcampan): figure out why removing this prevents tabs opened in the
  //                background from properly taking focus.
  // We NULL-check the render_view_host_ here because Windows can send us
  // messages during the destruction process after it has been destroyed.
  if (web_contents()->render_widget_host_view()) {
    HWND inner_hwnd =
        web_contents()->render_widget_host_view()->GetPluginNativeView();
    if (::IsWindow(inner_hwnd))
      ::SetFocus(inner_hwnd);
  }
}

void TabContentsViewWin::OnVScroll(int scroll_type, short position,
                                   HWND scrollbar) {
  ScrollCommon(WM_VSCROLL, scroll_type, position, scrollbar);
}

void TabContentsViewWin::OnWindowPosChanged(WINDOWPOS* window_pos) {
  if (window_pos->flags & SWP_HIDEWINDOW) {
    WasHidden();
  } else {
    // The WebContents was shown by a means other than the user selecting a
    // Tab, e.g. the window was minimized then restored.
    if (window_pos->flags & SWP_SHOWWINDOW)
      WasShown();

    // Unless we were specifically told not to size, cause the renderer to be
    // sized to the new bounds, which forces a repaint. Not required for the
    // simple minimize-restore case described above, for example, since the
    // size hasn't changed.
    if (!(window_pos->flags & SWP_NOSIZE))
      WasSized(gfx::Size(window_pos->cx, window_pos->cy));
  }
}

void TabContentsViewWin::OnSize(UINT param, const CSize& size) {
  WidgetWin::OnSize(param, size);

  // Hack for thinkpad touchpad driver.
  // Set fake scrollbars so that we can get scroll messages,
  SCROLLINFO si = {0};
  si.cbSize = sizeof(si);
  si.fMask = SIF_ALL;

  si.nMin = 1;
  si.nMax = 100;
  si.nPage = 10;
  si.nPos = 50;

  ::SetScrollInfo(GetNativeView(), SB_HORZ, &si, FALSE);
  ::SetScrollInfo(GetNativeView(), SB_VERT, &si, FALSE);
}

LRESULT TabContentsViewWin::OnNCCalcSize(BOOL w_param, LPARAM l_param) {
  // Hack for thinkpad mouse wheel driver. We have set the fake scroll bars
  // to receive scroll messages from thinkpad touchpad driver. Suppress
  // painting of scrollbars by returning 0 size for them.
  return 0;
}

void TabContentsViewWin::OnNCPaint(HRGN rgn) {
  // Suppress default WM_NCPAINT handling. We don't need to do anything
  // here since the view will draw everything correctly.
}

void TabContentsViewWin::ScrollCommon(UINT message, int scroll_type,
                                      short position, HWND scrollbar) {
  // This window can receive scroll events as a result of the ThinkPad's
  // Trackpad scroll wheel emulation.
  if (!ScrollZoom(scroll_type)) {
    // Reflect scroll message to the view() to give it a chance
    // to process scrolling.
    SendMessage(GetContentNativeView(), message,
                MAKELONG(scroll_type, position),
                reinterpret_cast<LPARAM>(scrollbar));
  }
}

void TabContentsViewWin::WasHidden() {
  web_contents()->HideContents();
}

void TabContentsViewWin::WasShown() {
  web_contents()->ShowContents();
}

void TabContentsViewWin::WasSized(const gfx::Size& size) {
  if (web_contents()->interstitial_page())
    web_contents()->interstitial_page()->SetSize(size);
  if (web_contents()->render_widget_host_view())
    web_contents()->render_widget_host_view()->SetSize(size);

  // TODO(brettw) this function can probably be moved to this class.
  web_contents()->RepositionSupressedPopupsToFit(size);
}

bool TabContentsViewWin::ScrollZoom(int scroll_type) {
  // If ctrl is held, zoom the UI.  There are three issues with this:
  // 1) Should the event be eaten or forwarded to content?  We eat the event,
  //    which is like Firefox and unlike IE.
  // 2) Should wheel up zoom in or out?  We zoom in (increase font size), which
  //    is like IE and Google maps, but unlike Firefox.
  // 3) Should the mouse have to be over the content area?  We zoom as long as
  //    content has focus, although FF and IE require that the mouse is over
  //    content.  This is because all events get forwarded when content has
  //    focus.
  if (GetAsyncKeyState(VK_CONTROL) & 0x8000) {
    int distance = 0;
    switch (scroll_type) {
      case SB_LINEUP:
        distance = WHEEL_DELTA;
        break;
      case SB_LINEDOWN:
        distance = -WHEEL_DELTA;
        break;
        // TODO(joshia): Handle SB_PAGEUP, SB_PAGEDOWN, SB_THUMBPOSITION,
        // and SB_THUMBTRACK for completeness
      default:
        break;
    }

    WheelZoom(distance);
    return true;
  }
  return false;
}

void TabContentsViewWin::WheelZoom(int distance) {
  if (web_contents()->delegate()) {
    bool zoom_in = distance > 0;
    web_contents()->delegate()->ContentsZoomChange(zoom_in);
  }
}
