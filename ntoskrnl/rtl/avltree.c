/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     AVL trees of RTL_BALANCED_NODE entries
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* DEFINES ********************************************************************/

/*
 * The balance is kept in the low two bits of ParentValue as a signed
 * two bit number: 0 is even, 1 leans right and 3 (-1) leans left.
 */
#define RTLP_AVL_EVEN           0u
#define RTLP_AVL_LEANS_RIGHT    1u
#define RTLP_AVL_LEANS_LEFT     3u

/* Balance of a node whose subtree on the given side is the taller one */
#define RTLP_AVL_LEANS(Side)    ((Side) ? RTLP_AVL_LEANS_RIGHT : RTLP_AVL_LEANS_LEFT)

/* PRIVATE FUNCTIONS **********************************************************/

FORCEINLINE
PRTL_BALANCED_NODE
RtlpAvlGetParent(
    _In_ PRTL_BALANCED_NODE Node)
{
    return RTL_BALANCED_NODE_GET_PARENT_POINTER(Node);
}

FORCEINLINE
VOID
RtlpAvlSetParent(
    _Inout_ PRTL_BALANCED_NODE Node,
    _In_opt_ PRTL_BALANCED_NODE Parent)
{
    Node->ParentValue = (ULONG_PTR)Parent |
                        (Node->ParentValue & RTL_BALANCED_NODE_RESERVED_PARENT_MASK);
}

FORCEINLINE
ULONG
RtlpAvlGetBalance(
    _In_ PRTL_BALANCED_NODE Node)
{
    return (ULONG)(Node->ParentValue & RTL_BALANCED_NODE_RESERVED_PARENT_MASK);
}

FORCEINLINE
VOID
RtlpAvlSetBalance(
    _Inout_ PRTL_BALANCED_NODE Node,
    _In_ ULONG Balance)
{
    Node->ParentValue = (Node->ParentValue & ~(ULONG_PTR)RTL_BALANCED_NODE_RESERVED_PARENT_MASK) |
                        Balance;
}

/**
 * @brief
 * Tells which child of its parent a node is. The parent must not be NULL.
 *
 * @return
 * 1 if the node is the right child, 0 if it is the left one.
 */
static
ULONG
RtlpAvlSideOf(
    _In_ PRTL_BALANCED_NODE Parent,
    _In_ PRTL_BALANCED_NODE Node)
{
    if (Parent->Right == Node)
        return 1;

    if (Parent->Left != Node)
        __fastfail(FAST_FAIL_INVALID_BALANCED_TREE);

    return 0;
}

/**
 * @brief
 * Points whatever referenced OldChild, its parent or the tree root, at NewChild.
 */
static
VOID
RtlpAvlReplaceChild(
    _Inout_ PRTL_AVL_TREE Tree,
    _Inout_opt_ PRTL_BALANCED_NODE Parent,
    _In_ PRTL_BALANCED_NODE OldChild,
    _In_opt_ PRTL_BALANCED_NODE NewChild)
{
    if (Parent == NULL)
    {
        if (Tree->Root != OldChild)
            __fastfail(FAST_FAIL_INVALID_BALANCED_TREE);

        Tree->Root = NewChild;
        return;
    }

    Parent->Children[RtlpAvlSideOf(Parent, OldChild)] = NewChild;
}

/**
 * @brief
 * Lifts the child of Node on the given side into the place of Node.
 * Balances are left for the caller to fix.
 *
 * @return
 * The lifted child, now the root of the subtree.
 */
static
PRTL_BALANCED_NODE
RtlpAvlRotate(
    _Inout_ PRTL_AVL_TREE Tree,
    _Inout_ PRTL_BALANCED_NODE Node,
    _In_ ULONG Side)
{
    PRTL_BALANCED_NODE Child = Node->Children[Side];
    PRTL_BALANCED_NODE Inner = Child->Children[!Side];
    PRTL_BALANCED_NODE Parent = RtlpAvlGetParent(Node);

    if (RtlpAvlGetParent(Child) != Node)
        __fastfail(FAST_FAIL_INVALID_BALANCED_TREE);

    RtlpAvlReplaceChild(Tree, Parent, Node, Child);
    RtlpAvlSetParent(Child, Parent);

    /* The inner grandchild moves across to Node */
    Node->Children[Side] = Inner;
    if (Inner)
    {
        if (RtlpAvlGetParent(Inner) != Child)
            __fastfail(FAST_FAIL_INVALID_BALANCED_TREE);

        RtlpAvlSetParent(Inner, Node);
    }

    Child->Children[!Side] = Node;
    RtlpAvlSetParent(Node, Child);

    return Child;
}

/**
 * @brief
 * Rebalances Node, which is two levels taller on the given side, through a
 * single or a double rotation.
 *
 * @return
 * The new root of the subtree.
 */
static
PRTL_BALANCED_NODE
RtlpAvlRebalance(
    _Inout_ PRTL_AVL_TREE Tree,
    _Inout_ PRTL_BALANCED_NODE Node,
    _In_ ULONG Side,
    _Out_ PBOOLEAN HeightKept)
{
    PRTL_BALANCED_NODE Child = Node->Children[Side];
    PRTL_BALANCED_NODE Inner;
    ULONG ChildBalance = RtlpAvlGetBalance(Child);
    ULONG InnerBalance;

    *HeightKept = FALSE;

    if (ChildBalance != RTLP_AVL_LEANS(!Side))
    {
        RtlpAvlRotate(Tree, Node, Side);

        /* An even child only happens on removal, the subtree keeps its height */
        if (ChildBalance == RTLP_AVL_EVEN)
        {
            RtlpAvlSetBalance(Node, RTLP_AVL_LEANS(Side));
            RtlpAvlSetBalance(Child, RTLP_AVL_LEANS(!Side));
            *HeightKept = TRUE;
        }
        else
        {
            RtlpAvlSetBalance(Node, RTLP_AVL_EVEN);
            RtlpAvlSetBalance(Child, RTLP_AVL_EVEN);
        }

        return Child;
    }

    /* The child leans the other way, so its inner child becomes the new root */
    Inner = Child->Children[!Side];
    InnerBalance = RtlpAvlGetBalance(Inner);

    RtlpAvlRotate(Tree, Child, !Side);
    RtlpAvlRotate(Tree, Node, Side);

    RtlpAvlSetBalance(Node, (InnerBalance == RTLP_AVL_LEANS(Side)) ?
                            RTLP_AVL_LEANS(!Side) : RTLP_AVL_EVEN);
    RtlpAvlSetBalance(Child, (InnerBalance == RTLP_AVL_LEANS(!Side)) ?
                             RTLP_AVL_LEANS(Side) : RTLP_AVL_EVEN);
    RtlpAvlSetBalance(Inner, RTLP_AVL_EVEN);

    return Inner;
}

/* PUBLIC FUNCTIONS ***********************************************************/

/**
 * @brief
 * Links a node into an AVL tree below a known parent and rebalances the tree.
 *
 * @param[in,out] Tree
 * The tree to insert into.
 *
 * @param[in,out] Parent
 * The node that gets the new child, or NULL to make the node the root.
 * The child slot on the chosen side must be empty.
 *
 * @param[in] Right
 * TRUE to insert as the right child of @p Parent, FALSE for the left child.
 *
 * @param[out] Node
 * The node to insert. It is initialized by this routine.
 */
VOID
NTAPI
RtlAvlInsertNodeEx(
    _Inout_ PRTL_AVL_TREE Tree,
    _Inout_opt_ PRTL_BALANCED_NODE Parent,
    _In_ BOOLEAN Right,
    _Out_ PRTL_BALANCED_NODE Node)
{
    PRTL_BALANCED_NODE Grown = Node;
    PRTL_BALANCED_NODE Current = Parent;
    ULONG Side = Right ? 1 : 0;
    ULONG Balance;
    BOOLEAN HeightKept;

    Node->Left = NULL;
    Node->Right = NULL;
    Node->ParentValue = (ULONG_PTR)Parent;

    if (Parent == NULL)
    {
        Tree->Root = Node;
        return;
    }

    Parent->Children[Side] = Node;

    /* Walk up for as long as the subtree on the grown side got taller */
    for (;;)
    {
        Balance = RtlpAvlGetBalance(Current);

        if (Balance == RTLP_AVL_EVEN)
        {
            RtlpAvlSetBalance(Current, RTLP_AVL_LEANS(Side));

            Grown = Current;
            Current = RtlpAvlGetParent(Current);
            if (Current == NULL)
                return;

            Side = RtlpAvlSideOf(Current, Grown);
            continue;
        }

        if (Balance == RTLP_AVL_LEANS(!Side))
        {
            RtlpAvlSetBalance(Current, RTLP_AVL_EVEN);
            return;
        }

        /* A rotation after an insertion always restores the old height */
        RtlpAvlRebalance(Tree, Current, Side, &HeightKept);
        return;
    }
}

/**
 * @brief
 * Unlinks a node from an AVL tree and rebalances the tree.
 *
 * @param[in,out] Tree
 * The tree that holds the node.
 *
 * @param[in,out] Node
 * The node to remove. Its links are not cleared.
 *
 * @remarks
 * A node with two children is replaced by its in-order predecessor when it
 * leans left and by its in-order successor otherwise.
 */
VOID
NTAPI
RtlAvlRemoveNode(
    _Inout_ PRTL_AVL_TREE Tree,
    _Inout_ PRTL_BALANCED_NODE Node)
{
    PRTL_BALANCED_NODE Parent = RtlpAvlGetParent(Node);
    PRTL_BALANCED_NODE Current;
    PRTL_BALANCED_NODE Shrunk;
    PRTL_BALANCED_NODE Heir;
    PRTL_BALANCED_NODE HeirParent;
    PRTL_BALANCED_NODE Orphan;
    ULONG HeirSide;
    ULONG Side;
    ULONG Balance;
    BOOLEAN HeightKept;

    if (Node->Left && Node->Right)
    {
        /* Take the neighbor from the taller subtree, the right one on a tie */
        HeirSide = (RtlpAvlGetBalance(Node) == RTLP_AVL_LEANS_LEFT) ? 0 : 1;

        Heir = Node->Children[HeirSide];
        while (Heir->Children[!HeirSide])
            Heir = Heir->Children[!HeirSide];

        HeirParent = RtlpAvlGetParent(Heir);
        if (HeirParent == Node)
        {
            /* A direct child keeps its own subtree on that side */
            Current = Heir;
            Side = HeirSide;
        }
        else
        {
            /* Hand the heir's only child to its parent, then adopt the subtree of Node */
            if (HeirParent->Children[!HeirSide] != Heir)
                __fastfail(FAST_FAIL_INVALID_BALANCED_TREE);

            Orphan = Heir->Children[HeirSide];
            HeirParent->Children[!HeirSide] = Orphan;
            if (Orphan)
                RtlpAvlSetParent(Orphan, HeirParent);

            Heir->Children[HeirSide] = Node->Children[HeirSide];
            RtlpAvlSetParent(Heir->Children[HeirSide], Heir);

            Current = HeirParent;
            Side = !HeirSide;
        }

        Heir->Children[!HeirSide] = Node->Children[!HeirSide];
        RtlpAvlSetParent(Heir->Children[!HeirSide], Heir);

        /* The heir takes over both the position and the balance of Node */
        Heir->ParentValue = Node->ParentValue;
        RtlpAvlReplaceChild(Tree, Parent, Node, Heir);
    }
    else
    {
        Orphan = Node->Left ? Node->Left : Node->Right;
        if (Orphan)
            RtlpAvlSetParent(Orphan, Parent);

        if (Parent == NULL)
        {
            RtlpAvlReplaceChild(Tree, NULL, Node, Orphan);
            return;
        }

        Side = RtlpAvlSideOf(Parent, Node);
        Parent->Children[Side] = Orphan;
        Current = Parent;
    }

    /* Walk up for as long as the subtree on the shrunk side got shorter */
    for (;;)
    {
        Balance = RtlpAvlGetBalance(Current);

        if (Balance == RTLP_AVL_EVEN)
        {
            RtlpAvlSetBalance(Current, RTLP_AVL_LEANS(!Side));
            return;
        }

        if (Balance == RTLP_AVL_LEANS(Side))
        {
            RtlpAvlSetBalance(Current, RTLP_AVL_EVEN);
            Shrunk = Current;
        }
        else
        {
            Shrunk = RtlpAvlRebalance(Tree, Current, !Side, &HeightKept);
            if (HeightKept)
                return;
        }

        Current = RtlpAvlGetParent(Shrunk);
        if (Current == NULL)
            return;

        Side = RtlpAvlSideOf(Current, Shrunk);
    }
}

/* EOF */
