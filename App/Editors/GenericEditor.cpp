#include "GenericEditor.h"
#include "../PackageWindow.h"
#include "../App.h"

#include <Tera/ALog.h>
#include <Tera/UObject.h>

#include "ObjectRedirectorEditor.h"
#include "TextureEditor.h"

GenericEditor* GenericEditor::CreateEditor(wxPanel* parent, PackageWindow* window, UObject* object)
{
  GenericEditor* editor = nullptr;
  if (object->GetClassName() == UObjectRedirector::StaticClassName())
  {
    editor = new ObjectRedirectorEditor(parent, window);
  }
  else if (object->GetClassName() == UTexture2D::StaticClassName())
  {
    editor = new TextureEditor(parent, window);
  }
  else
  {
    editor = new GenericEditor(parent, window);
  }
  editor->SetObject(object);
  return editor;
}

GenericEditor::GenericEditor(wxPanel* parent, PackageWindow* window)
  : wxPanel(parent, wxID_ANY)
  , Window(window)
{
  SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_APPWORKSPACE));
}

void GenericEditor::LoadObject()
{
  if (IsLoading())
  {
    return;
  }
  std::string id = GetEditorId();
  if (!Object->IsLoaded())
  {
    Loading = true;
    UObject* obj = Object;
    std::weak_ptr<FPackage> wpackage = Window->GetPackage();
    std::thread([obj, id, wpackage] {
      if (auto l = wpackage.lock())
      {
        obj->Load();
        SendEvent(wxTheApp, OBJECT_LOADED, id);
      }
    }).detach();
    // TODO: some UI progress?
  }
  else
  {
    SendEvent(wxTheApp, OBJECT_LOADED, id);
  }
}

void GenericEditor::OnObjectLoaded()
{
  Loading = false;
}

std::string GenericEditor::GetEditorId() const
{
  return std::to_string((uint64)std::addressof(*this)) + "." + std::to_string((uint64)std::addressof(Object));
}

std::vector<FPropertyTag*> GenericEditor::GetObjectProperties()
{
  return Object->GetProperties();
}

void GenericEditor::PopulateToolBar(wxToolBar* toolbar)
{
  if (Object->GetDataSize())
  {
    toolbar->AddTool(eID_Export, "Export", wxBitmap("#112", wxBITMAP_TYPE_PNG_RESOURCE), "Export object data...");
    toolbar->AddTool(eID_Import, "Import", wxBitmap("#113", wxBITMAP_TYPE_PNG_RESOURCE), "Import object data...");
  }
}

void GenericEditor::OnToolBarEvent(wxCommandEvent& e)
{
  if (e.GetId() == eID_Export)
  {
    OnExportClicked(e);
    e.Skip(true);
  }
  else if (e.GetId() == eID_Import)
  {
    OnImportClicked(e);
    e.Skip(true);
  }
}

void GenericEditor::OnExportClicked(wxCommandEvent& e)
{
  wxString path = wxSaveFileSelector("raw object data", ".*", Object->GetObjectName().WString(), Window);

}

void GenericEditor::OnImportClicked(wxCommandEvent& e)
{
  wxString path = wxLoadFileSelector("raw object data", ".*", wxEmptyString, Window);
  // TODO: import data
}
