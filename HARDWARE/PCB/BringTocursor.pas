{ ============================================================
  BringToCursor.pas  –  Altium DelphiScript
  Bauteil per Designator an die aktuelle Cursor-Position holen
  Stefan Waltersberger  –  IO-Hutschienenboard
  ============================================================

  Installation:
    1. DXP → Run Script → dieses File wählen
    2. Funktion "BringToCursor" ausführen
    3. Optional: Shortcut via DXP → Customize → Scripting → BringToCursor

  Verwendung:
    - Script starten (oder Shortcut drücken)
    - Cursor an die gewünschte Stelle im PCB bewegen
    - Designator eingeben (z.B. R100)
    - Bauteil springt zum Cursor und bleibt zur Platzierung am Cursor hängen
  ============================================================ }

procedure BringToCursor;
var
  Board       : IPCB_Board;
  Comp        : IPCB_Component;
  Iterator    : IPCB_BoardIterator;
  Designator  : String;
  CursorX     : TCoord;
  CursorY     : TCoord;
  Found       : Boolean;
begin
  { PCB-Dokument holen }
  Board := PCBServer.GetCurrentPCBBoard;
  if Board = nil then
  begin
    ShowMessage('Kein PCB-Dokument aktiv!');
    Exit;
  end;

  { Cursor-Position merken (vor dem Dialog!) }
  CursorX := Board.XCursor;
  CursorY := Board.YCursor;

  { Designator abfragen }
  Designator := InputBox('Bauteil zum Cursor holen', 'Designator eingeben:', '');
  if Designator = '' then Exit;
  Designator := UpperCase(Trim(Designator));

  { Bauteil suchen }
  Found    := False;
  Iterator := Board.BoardIterator_Create;
  Iterator.AddFilter_ObjectSet(MkSet(eComponentObject));
  Iterator.AddFilter_LayerSet(AllLayers);

  Comp := Iterator.FirstPCBObject;
  while Comp <> nil do
  begin
    if UpperCase(Comp.Name.Text) = Designator then
    begin
      Found := True;
      Break;
    end;
    Comp := Iterator.NextPCBObject;
  end;
  Board.BoardIterator_Destroy(Iterator);

  if not Found then
  begin
    ShowMessage('Bauteil "' + Designator + '" nicht gefunden!');
    Exit;
  end;

  { Bauteil zur Cursor-Position verschieben }
  PCBServer.PreProcess;

  Comp.MoveToXY(CursorX, CursorY);
  Comp.GraphicallyInvalidate;

  PCBServer.PostProcess;

  { View NICHT springen – nur Status melden und Bauteil interaktiv weiter verschieben }
  Board.ViewManager_UpdateLayerTabs;
  Board.ViewManager_FullUpdate;

  { Bauteil jetzt interaktiv am Cursor: Move starten }
  ResetParameters;
  AddStringParameter('OBJECT', 'COMPONENT');
  AddStringParameter('ITEM',   Designator);
  RunProcess('PCB:MoveObject');
end;

{ Einstiegspunkt für Altium Script-Manager }
procedure TFormBringToCursor;
begin
  BringToCursor;
end;
