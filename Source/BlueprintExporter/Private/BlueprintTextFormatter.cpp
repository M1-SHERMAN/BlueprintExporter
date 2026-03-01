#include "BlueprintTextFormatter.h"

FString FBlueprintTextFormatter::Format(const FExportedBlueprint& Blueprint)
{
	TArray<FString> Lines;

	// Header
	if (!Blueprint.ParentClass.IsEmpty())
	{
		Lines.Add(FString::Printf(TEXT("=== Blueprint: %s (Parent: %s) ==="),
			*Blueprint.BlueprintName, *Blueprint.ParentClass));
	}
	else
	{
		Lines.Add(FString::Printf(TEXT("=== Blueprint: %s ==="), *Blueprint.BlueprintName));
	}
	Lines.Add(TEXT(""));

	// Variables section
	if (Blueprint.Variables.Num() > 0)
	{
		FString VarSection = FormatVariables(Blueprint.Variables);
		if (!VarSection.IsEmpty())
		{
			Lines.Add(VarSection);
			Lines.Add(TEXT(""));
		}
	}

	for (const FExportedGraph& Graph : Blueprint.Graphs)
	{
		FString GraphOutput = FormatGraph(Graph);
		if (!GraphOutput.IsEmpty())
		{
			Lines.Add(GraphOutput);
		}
	}

	return FString::Join(Lines, TEXT("\n")).TrimEnd();
}

FString FBlueprintTextFormatter::FormatVariables(const TArray<FExportedVariable>& Variables)
{
	if (Variables.Num() == 0)
	{
		return FString();
	}

	// Calculate column widths for alignment
	int32 MaxNameWidth = 4;  // "Name"
	int32 MaxTypeWidth = 4;  // "Type"
	for (const FExportedVariable& Var : Variables)
	{
		MaxNameWidth = FMath::Max(MaxNameWidth, Var.Name.Len());

		FString FullType = Var.Type;
		if (!Var.ContainerType.IsEmpty())
		{
			FullType = FString::Printf(TEXT("%s<%s>"), *Var.ContainerType, *Var.Type);
		}
		MaxTypeWidth = FMath::Max(MaxTypeWidth, FullType.Len());
	}

	TArray<FString> Lines;
	Lines.Add(TEXT("=== Variables ==="));

	for (const FExportedVariable& Var : Variables)
	{
		FString FullType = Var.Type;
		if (!Var.ContainerType.IsEmpty())
		{
			FullType = FString::Printf(TEXT("%s<%s>"), *Var.ContainerType, *Var.Type);
		}

		FString NamePadded = Var.Name + FString::ChrN(MaxNameWidth - Var.Name.Len(), TEXT(' '));
		FString TypePadded = FullType + FString::ChrN(MaxTypeWidth - FullType.Len(), TEXT(' '));

		FString Line = FString::Printf(TEXT("  %s : %s"), *NamePadded, *TypePadded);

		if (!Var.DefaultValue.IsEmpty() && !IsTrivialDefault(Var.DefaultValue))
		{
			Line += FString::Printf(TEXT(" = %s"), *Var.DefaultValue);
		}

		// Omit flags if they are the default combination (EditDefaultsOnly + BlueprintReadWrite)
		bool bIsDefaultFlagCombo = (Var.Flags.Num() == 2
			&& Var.Flags.Contains(TEXT("EditDefaultsOnly"))
			&& Var.Flags.Contains(TEXT("BlueprintReadWrite")));

		if (Var.Flags.Num() > 0 && !bIsDefaultFlagCombo)
		{
			Line += FString::Printf(TEXT("  [%s]"), *FString::Join(Var.Flags, TEXT(", ")));
		}

		Lines.Add(Line);
	}

	return FString::Join(Lines, TEXT("\n"));
}

FString FBlueprintTextFormatter::FormatGraph(const FExportedGraph& Graph)
{
	if (Graph.Nodes.Num() == 0)
	{
		return FString();
	}

	TArray<FString> Lines;

	// Graph header
	if (Graph.GraphType == TEXT("EventGraph"))
	{
		Lines.Add(FString::Printf(TEXT("--- Graph: %s ---"), *Graph.GraphName));
	}
	else
	{
		Lines.Add(FString::Printf(TEXT("--- Graph: %s (%s) ---"),
			*Graph.GraphName, *Graph.GraphType));
	}
	Lines.Add(TEXT(""));

	// Build node map
	TMap<FString, const FExportedNode*> NodeMap;
	for (const FExportedNode& Node : Graph.Nodes)
	{
		NodeMap.Add(Node.NodeName, &Node);
	}

	// Topological sort
	TArray<FExportedNode> SortedNodes = TopologicalSort(Graph.Nodes);

	// Format each node
	for (const FExportedNode& Node : SortedNodes)
	{
		if (Node.NodeClass == TEXT("K2Node_Knot"))
		{
			continue;
		}
		Lines.Add(FormatNode(Node));
	}

	// Execution flow
	FString ExecFlow = FormatExecutionFlow(SortedNodes, NodeMap);
	if (!ExecFlow.IsEmpty())
	{
		Lines.Add(TEXT(""));                         // node block → Flow separator
		Lines.Add(TEXT("=== Execution Flow ==="));
		Lines.Add(ExecFlow);
		Lines.Add(TEXT(""));                         // Flow trailing blank (Graph separator)

		// Data flow
		FString DataFlow = FormatDataFlow(SortedNodes, NodeMap);
		if (!DataFlow.IsEmpty())
		{
			Lines.Add(TEXT("=== Data Flow ==="));
			Lines.Add(DataFlow);
			Lines.Add(TEXT(""));                     // trailing blank (Graph separator)
		}
	}
	else
	{
		Lines.Add(TEXT(""));                         // no Flow: keep trailing blank (Graph separator)
	}

	return FString::Join(Lines, TEXT("\n"));
}

FString FBlueprintTextFormatter::FormatNode(const FExportedNode& Node)
{
	TArray<FString> Lines;

	// Node title
	FString ReadableType = GetReadableType(Node.NodeClass);
	Lines.Add(FString::Printf(TEXT("[%s] %s"), *Node.NodeName, *ReadableType));

	// Node properties (TArray<TPair> preserves insertion order)
	for (const auto& Prop : Node.Properties)
	{
		if (Prop.Key == TEXT("SelfContext"))
		{
			continue;
		}
		if (Prop.Key == TEXT("Override") && Prop.Value == TEXT("true"))
		{
			Lines.Add(TEXT("  (Override)"));
		}
		else
		{
			Lines.Add(FString::Printf(TEXT("  %s: %s"), *Prop.Key, *Prop.Value));
		}
	}

	// Meaningful pins
	TArray<FExportedPin> VisiblePins = GetMeaningfulPins(Node);
	for (const FExportedPin& Pin : VisiblePins)
	{
		FString PinStr = FormatPin(Pin);
		if (!PinStr.IsEmpty())
		{
			Lines.Add(FString::Printf(TEXT("  %s"), *PinStr));
		}
	}

	// Render expanded subgraph for Composite nodes
	if (Node.SubGraph)
	{
		FString SubContent = FormatGraph(*Node.SubGraph);
		if (!SubContent.IsEmpty())
		{
			TArray<FString> SubLines;
			SubContent.ParseIntoArrayLines(SubLines);
			for (const FString& SubLine : SubLines)
			{
				Lines.Add(FString::Printf(TEXT("  %s"), *SubLine));
			}
		}
	}

	return FString::Join(Lines, TEXT("\n"));
}

FString FBlueprintTextFormatter::FormatPin(const FExportedPin& Pin)
{
	// Direction arrow
	const TCHAR* Arrow = Pin.Direction == TEXT("Output") ? TEXT("\u2192") : TEXT("\u2190");

	// Type string: prefer SubType, fallback to Category
	FString TypeStr = Pin.Category;
	if (!Pin.SubType.IsEmpty())
	{
		TypeStr = Pin.SubType;
	}
	if (!Pin.ContainerType.IsEmpty())
	{
		TypeStr = FString::Printf(TEXT("%s<%s>"), *Pin.ContainerType, *TypeStr);
	}

	// Default value: only show non-trivial defaults; exec/delegate never show
	FString Default;
	if (!Pin.DefaultValue.IsEmpty()
		&& !IsTrivialDefault(Pin.DefaultValue)
		&& Pin.Category != TEXT("exec")
		&& Pin.Category != TEXT("delegate"))
	{
		Default = FString::Printf(TEXT(" = %s"), *Pin.DefaultValue);
	}

	// Connection targets
	FString Connection;
	if (Pin.LinkedTo.Num() > 0)
	{
		TArray<FString> Targets;
		for (const auto& Link : Pin.LinkedTo)
		{
			FString TargetStr = FString::Printf(TEXT("%s.%s"), *Link.Key, *Link.Value);
			if (!SelectionContext.IsEmpty() && !SelectionContext.Contains(Link.Key))
			{
				TargetStr += TEXT(" (external)");
			}
			Targets.Add(TargetStr);
		}
		Connection = FString::Printf(TEXT(" -> %s"), *FString::Join(Targets, TEXT(", ")));
	}

	return FString::Printf(TEXT("%s %s (%s)%s%s"),
		Arrow, *Pin.Name, *TypeStr, *Default, *Connection);
}

FString FBlueprintTextFormatter::GetReadableType(const FString& NodeClass)
{
	static TMap<FString, FString> TypeMap;
	if (TypeMap.Num() == 0)
	{
		TypeMap.Add(TEXT("K2Node_Event"), TEXT("EVENT"));
		TypeMap.Add(TEXT("K2Node_CustomEvent"), TEXT("CUSTOM_EVENT"));
		TypeMap.Add(TEXT("K2Node_CallFunction"), TEXT("CALL"));
		TypeMap.Add(TEXT("K2Node_CallArrayFunction"), TEXT("CALL(Array)"));
		TypeMap.Add(TEXT("K2Node_VariableGet"), TEXT("GET"));
		TypeMap.Add(TEXT("K2Node_VariableSet"), TEXT("SET"));
		TypeMap.Add(TEXT("K2Node_IfThenElse"), TEXT("BRANCH"));
		TypeMap.Add(TEXT("K2Node_SwitchEnum"), TEXT("SWITCH"));
		TypeMap.Add(TEXT("K2Node_SwitchInteger"), TEXT("SWITCH(Int)"));
		TypeMap.Add(TEXT("K2Node_SwitchString"), TEXT("SWITCH(String)"));
		TypeMap.Add(TEXT("K2Node_MacroInstance"), TEXT("MACRO"));
		TypeMap.Add(TEXT("K2Node_DynamicCast"), TEXT("CAST"));
		TypeMap.Add(TEXT("K2Node_Knot"), TEXT("REROUTE"));
		TypeMap.Add(TEXT("K2Node_FunctionEntry"), TEXT("FUNC_ENTRY"));
		TypeMap.Add(TEXT("K2Node_FunctionResult"), TEXT("FUNC_RESULT"));
		TypeMap.Add(TEXT("K2Node_MakeArray"), TEXT("MAKE_ARRAY"));
		TypeMap.Add(TEXT("K2Node_MakeStruct"), TEXT("MAKE_STRUCT"));
		TypeMap.Add(TEXT("K2Node_BreakStruct"), TEXT("BREAK_STRUCT"));
		TypeMap.Add(TEXT("K2Node_Select"), TEXT("SELECT"));
		TypeMap.Add(TEXT("K2Node_SpawnActorFromClass"), TEXT("SPAWN_ACTOR"));
		TypeMap.Add(TEXT("K2Node_Timeline"), TEXT("TIMELINE"));
		TypeMap.Add(TEXT("K2Node_Delay"), TEXT("DELAY"));
		TypeMap.Add(TEXT("K2Node_ForEachLoop"), TEXT("FOR_EACH"));
		TypeMap.Add(TEXT("K2Node_CommutativeAssociativeBinaryOperator"), TEXT("OPERATOR"));
		TypeMap.Add(TEXT("K2Node_PromotableOperator"), TEXT("OPERATOR"));
		TypeMap.Add(TEXT("K2Node_ComponentBoundEvent"), TEXT("COMPONENT_EVENT"));
		TypeMap.Add(TEXT("K2Node_Composite"), TEXT("COLLAPSED"));
		TypeMap.Add(TEXT("K2Node_Tunnel"), TEXT("TUNNEL"));
	}

	const FString* Found = TypeMap.Find(NodeClass);
	if (Found)
	{
		return *Found;
	}

	// Fallback: strip K2Node_ prefix
	FString Result = NodeClass;
	Result.RemoveFromStart(TEXT("K2Node_"));
	return Result;
}

TArray<FExportedNode> FBlueprintTextFormatter::TopologicalSort(const TArray<FExportedNode>& Nodes)
{
	// Build node name to index map
	TMap<FString, int32> NameToIndex;
	for (int32 i = 0; i < Nodes.Num(); ++i)
	{
		NameToIndex.Add(Nodes[i].NodeName, i);
	}

	// Build exec flow directed graph and compute in-degrees
	TMap<int32, TArray<int32>> ExecGraph;
	TMap<int32, int32> InDegree;

	for (int32 i = 0; i < Nodes.Num(); ++i)
	{
		ExecGraph.FindOrAdd(i);
		InDegree.FindOrAdd(i) = 0;
	}

	for (int32 i = 0; i < Nodes.Num(); ++i)
	{
		const FExportedNode& Node = Nodes[i];
		for (const FExportedPin& Pin : Node.Pins)
		{
			if (Pin.Category == TEXT("exec") && Pin.Direction == TEXT("Output") && Pin.LinkedTo.Num() > 0)
			{
				for (const auto& Link : Pin.LinkedTo)
				{
					const int32* TargetIdx = NameToIndex.Find(Link.Key);
					if (TargetIdx)
					{
						ExecGraph[i].Add(*TargetIdx);
						InDegree[*TargetIdx]++;
					}
				}
			}
		}
	}

	// Kahn's algorithm
	TArray<int32> Queue;
	for (const auto& Pair : InDegree)
	{
		if (Pair.Value == 0)
		{
			Queue.Add(Pair.Key);
		}
	}

	TArray<int32> SortedIndices;
	while (Queue.Num() > 0)
	{
		// Sort queue: Event nodes first, then by name
		Queue.Sort([&Nodes](int32 A, int32 B)
		{
			bool AIsEvent = Nodes[A].NodeClass.Contains(TEXT("Event"));
			bool BIsEvent = Nodes[B].NodeClass.Contains(TEXT("Event"));
			if (AIsEvent != BIsEvent)
			{
				return AIsEvent; // Events come first
			}
			return Nodes[A].NodeName < Nodes[B].NodeName;
		});

		int32 Current = Queue[0];
		Queue.RemoveAt(0);
		SortedIndices.Add(Current);

		for (int32 Neighbor : ExecGraph[Current])
		{
			InDegree[Neighbor]--;
			if (InDegree[Neighbor] == 0)
			{
				Queue.Add(Neighbor);
			}
		}
	}

	// Append remaining nodes not in sorted result (cycles or disconnected)
	TSet<int32> SortedSet(SortedIndices);
	for (int32 i = 0; i < Nodes.Num(); ++i)
	{
		if (!SortedSet.Contains(i))
		{
			SortedIndices.Add(i);
		}
	}

	TArray<FExportedNode> Result;
	for (int32 Idx : SortedIndices)
	{
		Result.Add(Nodes[Idx]);
	}
	return Result;
}

TArray<FExportedPin> FBlueprintTextFormatter::GetMeaningfulPins(const FExportedNode& Node)
{
	TArray<FExportedPin> Meaningful;

	for (const FExportedPin& Pin : Node.Pins)
	{
		// Always skip hidden self/Target pin
		if (Pin.bIsHidden && (Pin.Name == TEXT("Target") || Pin.Name == TEXT("self")))
		{
			continue;
		}

		// Skip Output_Get pin if not connected
		if (Pin.Name == TEXT("Output_Get") && Pin.LinkedTo.Num() == 0)
		{
			continue;
		}

		// Skip all pins of Reroute nodes (shown in flow summary instead)
		if (Node.NodeClass == TEXT("K2Node_Knot"))
		{
			continue;
		}

		// Skip exec input pins — exec output from source already declares this connection
		if (Pin.Category == TEXT("exec") && Pin.Direction == TEXT("Input"))
		{
			continue;
		}

		// Exec pins with no connections
		if (Pin.Category == TEXT("exec") && Pin.LinkedTo.Num() == 0)
		{
			// Keep named exec outputs (e.g. Branch true/false, Is Valid/Is Not Valid)
			static const TSet<FString> StandardExecNames = {
				TEXT("execute"), TEXT("then"), TEXT("OutputDelegate")
			};
			if (!StandardExecNames.Contains(Pin.Name))
			{
				Meaningful.Add(Pin);
			}
			continue;
		}

		// Skip internal engine pins (e.g. Default__KismetMathLibrary on SwitchEnum)
		if (Pin.DefaultValue.StartsWith(TEXT("Default__")) && Pin.LinkedTo.Num() == 0)
		{
			continue;
		}

		// Skip LatentActionInfo pins — always present on latent nodes, default value is meaningless
		if (Pin.SubType == TEXT("LatentActionInfo") && Pin.LinkedTo.Num() == 0)
		{
			continue;
		}

		// Skip unconnected delegate pins (e.g. CustomEvent Output Delegate)
		if (Pin.Category == TEXT("delegate") && Pin.LinkedTo.Num() == 0)
		{
			continue;
		}

		// Skip hidden + no connection + no default value pins
		if (Pin.bIsHidden && Pin.LinkedTo.Num() == 0 && Pin.DefaultValue.IsEmpty())
		{
			continue;
		}

		// Skip unconnected non-exec output pins with no meaningful default (not consumed by any node)
		if (Pin.Direction == TEXT("Output")
			&& Pin.Category != TEXT("exec")
			&& Pin.LinkedTo.Num() == 0
			&& (Pin.DefaultValue.IsEmpty() || IsTrivialDefault(Pin.DefaultValue)))
		{
			continue;
		}

		Meaningful.Add(Pin);
	}

	return Meaningful;
}

bool FBlueprintTextFormatter::IsTrivialDefault(const FString& Value)
{
	static const TSet<FString> TrivialValues = {
		TEXT("0"),
		TEXT("0.0"),
		TEXT("0.000000"),
		TEXT("0, 0, 0"),
		TEXT("false"),
		TEXT("None"),
		TEXT("()"),                                    // empty struct / delegate
		TEXT("(())"),                                  // empty array
		TEXT("(X=0.000000,Y=0.000000,Z=0.000000)"),   // zero vector
	};
	return TrivialValues.Contains(Value);
}

TPair<FString, FString> FBlueprintTextFormatter::ResolveRerouteChain(const FString& NodeName, const FString& OriginalPinName, const TMap<FString, const FExportedNode*>& NodeMap)
{
	TSet<FString> Visited;
	FString Current = NodeName;
	FString CurrentPinName = OriginalPinName;

	while (true)
	{
		const FExportedNode* const* NodePtr = NodeMap.Find(Current);
		if (!NodePtr || !*NodePtr)
		{
			break;
		}

		const FExportedNode& Node = **NodePtr;
		if (Node.NodeClass != TEXT("K2Node_Knot"))
		{
			break;
		}

		if (Visited.Contains(Current))
		{
			break;
		}
		Visited.Add(Current);

		// Find the output pin and follow it
		bool bFoundNext = false;
		for (const FExportedPin& Pin : Node.Pins)
		{
			if (Pin.Direction == TEXT("Output") && Pin.LinkedTo.Num() > 0)
			{
				Current = Pin.LinkedTo[0].Key;
				CurrentPinName = Pin.LinkedTo[0].Value;
				bFoundNext = true;
				break;
			}
		}

		if (!bFoundNext)
		{
			break;
		}
	}

	return TPair<FString, FString>(Current, CurrentPinName);
}

FString FBlueprintTextFormatter::FormatExecutionFlow(const TArray<FExportedNode>& Nodes, const TMap<FString, const FExportedNode*>& NodeMap)
{
	TArray<FString> Lines;

	for (const FExportedNode& Node : Nodes)
	{
		if (Node.NodeClass == TEXT("K2Node_Knot"))
		{
			continue;
		}

		for (const FExportedPin& Pin : Node.Pins)
		{
			if (Pin.Category != TEXT("exec") || Pin.Direction != TEXT("Output") || Pin.LinkedTo.Num() == 0)
			{
				continue;
			}

			for (const auto& Link : Pin.LinkedTo)
			{
				// Resolve reroute chain
				auto [FinalTarget, _] = ResolveRerouteChain(Link.Key, Link.Value, NodeMap);

				// In selection mode, skip connections to external nodes
				if (!SelectionContext.IsEmpty() && !SelectionContext.Contains(FinalTarget))
				{
					continue;
				}

				FString Label;
				if (Pin.Name != TEXT("then") && Pin.Name != TEXT("OutputPin"))
				{
					Label = FString::Printf(TEXT(" [%s]"), *Pin.Name);
				}

				Lines.Add(FString::Printf(TEXT("  %s%s --> %s"),
					*Node.NodeName, *Label, *FinalTarget));
			}
		}
	}

	return FString::Join(Lines, TEXT("\n"));
}

FString FBlueprintTextFormatter::FormatDataFlow(const TArray<FExportedNode>& Nodes, const TMap<FString, const FExportedNode*>& NodeMap)
{
	TArray<FString> Lines;

	for (const FExportedNode& Node : Nodes)
	{
		if (Node.NodeClass == TEXT("K2Node_Knot"))
		{
			continue;
		}

		for (const FExportedPin& Pin : Node.Pins)
		{
			// Non-exec, output, connected, not hidden
			if (Pin.Category == TEXT("exec") || Pin.Direction != TEXT("Output") ||
				Pin.LinkedTo.Num() == 0 || Pin.bIsHidden)
			{
				continue;
			}

			// Skip Output_Get and OutputDelegate
			if (Pin.Name == TEXT("Output_Get") || Pin.Name == TEXT("OutputDelegate"))
			{
				continue;
			}

			for (const auto& Link : Pin.LinkedTo)
			{
				auto [FinalNode, FinalPin] = ResolveRerouteChain(Link.Key, Link.Value, NodeMap);

				// In selection mode, skip connections to external nodes
				if (!SelectionContext.IsEmpty() && !SelectionContext.Contains(FinalNode))
				{
					continue;
				}

				Lines.Add(FString::Printf(TEXT("  %s.%s --> %s.%s"),
					*Node.NodeName, *Pin.Name, *FinalNode, *FinalPin));
			}
		}
	}

	return FString::Join(Lines, TEXT("\n"));
}

FString FBlueprintTextFormatter::FormatGraphOnly(const FExportedGraph& Graph)
{
	// SelectionContext is empty (default), so FormatGraph runs in full mode
	return FormatGraph(Graph);
}

FString FBlueprintTextFormatter::FormatSummary(const FExportedBlueprint& Blueprint)
{
	TArray<FString> Lines;

	// Blueprint header (same style as Format())
	if (Blueprint.ParentClass.IsEmpty())
	{
		Lines.Add(FString::Printf(TEXT("=== Blueprint: %s ==="), *Blueprint.BlueprintName));
	}
	else
	{
		Lines.Add(FString::Printf(TEXT("=== Blueprint: %s (Parent: %s) ==="),
			*Blueprint.BlueprintName, *Blueprint.ParentClass));
	}
	Lines.Add(TEXT(""));

	// Variables section (reuse existing FormatVariables)
	if (Blueprint.Variables.Num() > 0)
	{
		FString VarSection = FormatVariables(Blueprint.Variables);
		if (!VarSection.IsEmpty())
		{
			Lines.Add(VarSection);
			Lines.Add(TEXT(""));
		}
	}

	// Graph list table
	Lines.Add(TEXT("=== Graphs ==="));

	int32 MaxNameWidth = 9; // minimum width
	for (const FExportedGraph& G : Blueprint.Graphs)
	{
		MaxNameWidth = FMath::Max(MaxNameWidth, G.GraphName.Len());
	}

	for (const FExportedGraph& G : Blueprint.Graphs)
	{
		FString NamePad = G.GraphName + FString::ChrN(MaxNameWidth - G.GraphName.Len(), TEXT(' '));
		Lines.Add(FString::Printf(TEXT("  %s  (%s)  %d nodes"),
			*NamePad, *G.GraphType, G.Nodes.Num()));
	}

	return FString::Join(Lines, TEXT("\n")).TrimEnd();
}

FString FBlueprintTextFormatter::FormatSelectedNodes(const FExportedGraph& Graph, const FString& BlueprintName)
{
	if (Graph.Nodes.Num() == 0)
	{
		return FString();
	}

	// Populate selection context with all node names in the selection
	SelectionContext.Empty();
	for (const FExportedNode& Node : Graph.Nodes)
	{
		SelectionContext.Add(Node.NodeName);
	}

	TArray<FString> Lines;

	// Header
	Lines.Add(FString::Printf(TEXT("=== Selected Nodes from: %s / %s (%d nodes) ==="),
		*BlueprintName, *Graph.GraphName, Graph.Nodes.Num()));
	Lines.Add(TEXT(""));

	// Build node map
	TMap<FString, const FExportedNode*> NodeMap;
	for (const FExportedNode& Node : Graph.Nodes)
	{
		NodeMap.Add(Node.NodeName, &Node);
	}

	// Topological sort
	TArray<FExportedNode> SortedNodes = TopologicalSort(Graph.Nodes);

	// Format each node
	for (const FExportedNode& Node : SortedNodes)
	{
		if (Node.NodeClass == TEXT("K2Node_Knot"))
		{
			continue;
		}
		Lines.Add(FormatNode(Node));
	}

	// Execution flow
	FString ExecFlow = FormatExecutionFlow(SortedNodes, NodeMap);
	if (!ExecFlow.IsEmpty())
	{
		Lines.Add(TEXT(""));
		Lines.Add(TEXT("=== Execution Flow ==="));
		Lines.Add(ExecFlow);
		Lines.Add(TEXT(""));

		FString DataFlow = FormatDataFlow(SortedNodes, NodeMap);
		if (!DataFlow.IsEmpty())
		{
			Lines.Add(TEXT("=== Data Flow ==="));
			Lines.Add(DataFlow);
			Lines.Add(TEXT(""));
		}
	}
	else
	{
		Lines.Add(TEXT(""));
	}

	// Clear selection context so subsequent Format() calls are not affected
	SelectionContext.Empty();

	return FString::Join(Lines, TEXT("\n")).TrimEnd();
}
